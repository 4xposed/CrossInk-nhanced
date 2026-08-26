(() => {
  'use strict';

  const SQLJS_WASM_URL = 'https://cdnjs.cloudflare.com/ajax/libs/sql.js/1.14.2/sql-wasm.wasm';
  const MAX_CARDS = 4096;
  const MAX_FIELD_TEXT_BYTES = 2048;
  const MAX_FIELDS_PER_SIDE = 8;
  const MAX_SIDE_TEXT_BYTES = 4096;
  const MAX_COMPONENT_BYTES = 96;
  const HEADER_SIZE = 32;
  const INDEX_SIZE = 29;
  const DECK_DIRECTORY = '/decks';
  const BLOCK_TAGS = new Set(['ADDRESS', 'ARTICLE', 'ASIDE', 'BLOCKQUOTE', 'DIV', 'DL', 'DT', 'DD', 'FIELDSET', 'FIGCAPTION', 'FIGURE', 'FOOTER', 'FORM', 'H1', 'H2', 'H3', 'H4', 'H5', 'H6', 'HEADER', 'HR', 'LI', 'MAIN', 'NAV', 'OL', 'P', 'PRE', 'SECTION', 'TABLE', 'TR', 'UL']);
  const MIN_INT32 = -0x80000000;
  const MAX_INT32 = 0x7fffffff;
  const MAX_UINT16 = 0xffff;
  const encoder = new TextEncoder();

  function sanitize(value, fallback) {
    const result = String(value || '')
      .replace(/[\\/:*?"<>|\x00-\x1f]/g, ' ')
      .replace(/\s+/g, ' ')
      .trim()
      .replace(/^\.+$/, '');
    return result || fallback;
  }

  function truncateUtf8(value, maxBytes) {
    let out = '';
    for (const codePoint of value) {
      const next = out + codePoint;
      if (encoder.encode(next).length > maxBytes) break;
      out = next;
    }
    return out;
  }

  function deckTitleForType(sourceTitle, noteTypeName) {
    const suffix = ' - ';
    const type = truncateUtf8(sanitize(noteTypeName, 'Note type'), 48) || 'Note type';
    const sourceBytes = MAX_COMPONENT_BYTES - 6 - encoder.encode(suffix + type).length;
    const source = truncateUtf8(sanitize(String(sourceTitle || '').replace(/\.apkg$/i, ''), 'Anki deck'), Math.max(1, sourceBytes)) || 'Anki deck';
    return `${source}${suffix}${type}`;
  }

  function outputName(value) {
    const suffix = '.cdeck';
    const stem = String(value || '').replace(/\.cdeck$/i, '');
    return `${truncateUtf8(sanitize(stem, 'Anki deck'), MAX_COMPONENT_BYTES - encoder.encode(suffix).length) || 'Anki deck'}${suffix}`;
  }

  function defaultDeckFilename(sourceFilename, noteTypeName) {
    return outputName(deckTitleForType(sourceFilename, noteTypeName));
  }

  function destinationKey(filename) {
    return outputName(filename).toLowerCase();
  }

  function duplicateDestinationNames(decks) {
    const duplicates = new Set();
    const seen = new Set();
    for (const deck of decks) {
      const key = destinationKey(deck.filename);
      if (seen.has(key)) duplicates.add(key);
      seen.add(key);
    }
    return duplicates;
  }

  function normalizeDecksForUpload(decks) {
    const normalized = decks.map((deck) => ({ ...deck, filename: outputName(deck.filename) }));
    if (duplicateDestinationNames(normalized).size) {
      throw new Error('Choose distinct deck filenames before uploading.');
    }
    return normalized;
  }

  function htmlToText(html) {
    const documentBody = new DOMParser().parseFromString(String(html || ''), 'text/html').body;
    let text = '';
    const addBreak = () => {
      if (text && !text.endsWith('\n')) text += '\n';
    };
    const visit = (node) => {
      if (node.nodeType === Node.TEXT_NODE) {
        text += node.nodeValue || '';
        return;
      }
      if (node.nodeType !== Node.ELEMENT_NODE) return;
      if (node.tagName === 'BR') {
        addBreak();
        return;
      }
      const block = BLOCK_TAGS.has(node.tagName);
      if (block) addBreak();
      for (const child of node.childNodes) visit(child);
      if (block) addBreak();
    };
    for (const child of documentBody.childNodes) visit(child);
    return text
      .replace(/\[sound:[^\]]*\]/gi, '')
      .replace(/\r\n?/g, '\n')
      .replace(/[ \t]+\n/g, '\n')
      .replace(/\n[ \t]+/g, '\n')
      .replace(/\n{3,}/g, '\n\n')
      .trim();
  }

  function hasClozeMarkup(fields) {
    return /{{c\d+::/i.test(fields);
  }

  function skip(reasons, reason) {
    reasons.set(reason, (reasons.get(reason) || 0) + 1);
  }

  function currentAnkiDay(crt) {
    if (!Number.isSafeInteger(crt) || crt <= 0 || crt > Date.now() / 1000) {
      throw new Error('The Anki collection has an invalid creation time.');
    }
    return Math.floor((Date.now() / 1000 - crt) / 86400);
  }

  function collectionCreationTime(db) {
    let statement;
    try {
      statement = db.prepare('SELECT crt FROM col');
      if (!statement.step()) throw new Error('The Anki collection has no creation time.');
      return Number(statement.getAsObject().crt);
    } finally {
      if (statement) statement.free();
    }
  }

  function cardKind(queue) {
    if (queue === 0) return 0;
    return queue === 2 ? 1 : 2;
  }

  function readNoteTypes(db) {
    let statement;
    try {
      statement = db.prepare('SELECT models FROM col');
      if (!statement.step()) throw new Error('The Anki collection has no note type metadata.');
      const models = JSON.parse(String(statement.getAsObject().models || ''));
      if (!models || typeof models !== 'object') throw new Error('The Anki collection has invalid note type metadata.');
      const noteTypes = new Map();
      for (const model of Object.values(models)) {
        const id = String(model && model.id || '');
        const name = String(model && model.name || '').trim();
        const fields = Array.isArray(model && model.flds)
          ? model.flds.map((field) => String(field && field.name || '').trim())
          : [];
        if (id && name && fields.length && fields.every(Boolean)) {
          noteTypes.set(id, { id, name, fields });
        }
      }
      if (!noteTypes.size) throw new Error('The Anki collection has no compatible note types.');
      return noteTypes;
    } catch (error) {
      if (error instanceof Error && error.message.startsWith('The Anki collection')) throw error;
      throw new Error('The Anki collection has invalid note type metadata.');
    } finally {
      if (statement) statement.free();
    }
  }

  function readEligibleCards(db, today, noteTypes) {
    const cards = [];
    const skipped = new Map();
    let statement;
    try {
      statement = db.prepare(
        'SELECT cards.id AS cardId, cards.ord AS cardOrd, cards.queue AS queue, cards.due AS due, cards.ivl AS ivl, notes.mid AS noteTypeId, notes.flds AS fields ' +
        'FROM cards INNER JOIN notes ON cards.nid = notes.id ORDER BY cards.due, cards.id'
      );
      while (statement.step()) {
        const row = statement.getAsObject();
        const queue = Number(row.queue);
        const order = Number(row.cardOrd);
        const due = Number(row.due);
        const interval = Number(row.ivl);
        if (!Number.isSafeInteger(queue) || !Number.isSafeInteger(order) || !Number.isSafeInteger(due) || !Number.isSafeInteger(interval)) {
          skip(skipped, 'unreadable scheduling data');
          continue;
        }
        if (queue < 0) {
          skip(skipped, 'suspended or buried card');
          continue;
        }
        if (order !== 0 && order !== 1) {
          skip(skipped, 'unsupported card direction');
          continue;
        }
        const noteTypeId = String(row.noteTypeId);
        if (!noteTypes.has(noteTypeId)) {
          skip(skipped, 'missing note type metadata');
          continue;
        }
        const fields = String(row.fields || '');
        if (hasClozeMarkup(fields)) {
          skip(skipped, 'cloze card');
          continue;
        }
        const sourceCardId = BigInt(row.cardId);
        if (sourceCardId <= 0n) {
          skip(skipped, 'unreadable card identifier');
          continue;
        }
        const kind = cardKind(queue);
        cards.push({
          sourceCardId,
          noteTypeId,
          fields: fields.split('\x1f'),
          dueOffset: kind === 0 ? due : kind === 1 ? Math.max(0, due - today) : 0,
          interval: kind === 1 ? Math.max(1, interval) : 1,
          order,
          kind
        });
      }
    } finally {
      if (statement) statement.free();
    }
    return { cards, skipped };
  }

  function resolveSideFieldBlocks(fields, mappings, side) {
    const blocks = [];
    const blockByText = new Map();
    let sideTextBytes = 0;
    for (const mapping of mappings) {
      if (mapping.side !== side) continue;
      const text = htmlToText(fields[mapping.index]);
      if (!text) continue;
      const existing = blockByText.get(text);
      if (existing) {
        existing.primary = existing.primary || mapping.primary === true;
        continue;
      }
      const bytes = encoder.encode(text);
      if (bytes.length > MAX_FIELD_TEXT_BYTES) {
        return { error: 'field text longer than 2,048 bytes' };
      }
      if (blocks.length === MAX_FIELDS_PER_SIDE) {
        return { error: 'more than 8 field blocks on one side' };
      }
      if (sideTextBytes > MAX_SIDE_TEXT_BYTES - bytes.length) {
        return { error: 'field blocks longer than 4,096 bytes on one side' };
      }
      const block = { bytes, primary: mapping.primary === true };
      blocks.push(block);
      blockByText.set(text, block);
      sideTextBytes += bytes.length;
    }
    if (side === 'front' && blocks.length === 1) {
      blocks[0].primary = true;
    }
    return { blocks };
  }

  function resolveTypeCards(eligibleCards, mapping) {
    const cards = [];
    const skipped = new Map();
    for (const card of eligibleCards) {
      const front = resolveSideFieldBlocks(card.fields, mapping.fields, 'front');
      const back = resolveSideFieldBlocks(card.fields, mapping.fields, 'back');
      if (front.error || back.error) {
        skip(skipped, front.error || back.error);
        continue;
      }
      if (!front.blocks.length || !back.blocks.length) {
        skip(skipped, 'empty prompt or answer');
        continue;
      }
      const promptBlocks = card.order === 0 ? front.blocks : back.blocks;
      const answerBlocks = card.order === 0 ? back.blocks : front.blocks;
      if (!Number.isSafeInteger(card.dueOffset) || card.dueOffset < MIN_INT32 || card.dueOffset > MAX_INT32) {
        skip(skipped, 'due offset outside signed 32-bit range');
        continue;
      }
      if (!Number.isSafeInteger(card.interval) || card.interval < 1 || card.interval > MAX_UINT16) {
        skip(skipped, 'interval outside unsigned 16-bit range');
        continue;
      }
      if (cards.length === MAX_CARDS) {
        skip(skipped, 'deck limit of 4,096 cards');
        continue;
      }
      cards.push({ ...card, promptBlocks, answerBlocks });
    }
    return { cards, skipped };
  }

  function randomDeckId() {
    const words = new Uint32Array(2);
    do {
      crypto.getRandomValues(words);
    } while (words[0] === 0 && words[1] === 0);
    return BigInt(words[0]) | (BigInt(words[1]) << 32n);
  }

  function sidePayloadLength(blocks) {
    return 1 + blocks.reduce((total, block) => total + 3 + block.bytes.length, 0);
  }

  function writeSidePayload(bytes, view, start, blocks) {
    bytes[start] = blocks.length;
    let cursor = start + 1;
    for (const block of blocks) {
      bytes[cursor] = block.primary ? 1 : 0;
      view.setUint16(cursor + 1, block.bytes.length, true);
      bytes.set(block.bytes, cursor + 3);
      cursor += 3 + block.bytes.length;
    }
    return cursor - start;
  }

  function buildDeck(title, cards) {
    const titleBytes = encoder.encode(title);
    const indexOffset = HEADER_SIZE + titleBytes.length;
    const textOffset = indexOffset + cards.length * INDEX_SIZE;
    const textLength = cards.reduce(
      (total, card) => total + sidePayloadLength(card.promptBlocks) + sidePayloadLength(card.answerBlocks),
      0
    );
    const bytes = new Uint8Array(textOffset + textLength);
    const view = new DataView(bytes.buffer);
    bytes.set([0x43, 0x4b, 0x44, 0x4b], 0);
    view.setUint16(4, 2, true);
    view.setUint16(6, HEADER_SIZE, true);
    view.setBigUint64(8, randomDeckId(), true);
    view.setUint32(16, cards.length, true);
    view.setUint16(20, titleBytes.length, true);
    view.setUint16(22, INDEX_SIZE, true);
    view.setUint32(24, indexOffset, true);
    view.setUint32(28, textOffset, true);
    bytes.set(titleBytes, HEADER_SIZE);

    let textCursor = 0;
    for (let index = 0; index < cards.length; index += 1) {
      const card = cards[index];
      const entry = indexOffset + index * INDEX_SIZE;
      view.setBigUint64(entry, card.sourceCardId, true);
      view.setUint32(entry + 8, textCursor, true);
      const promptLength = writeSidePayload(bytes, view, textOffset + textCursor, card.promptBlocks);
      view.setUint16(entry + 12, promptLength, true);
      textCursor += promptLength;
      view.setUint32(entry + 14, textCursor, true);
      const answerLength = writeSidePayload(bytes, view, textOffset + textCursor, card.answerBlocks);
      view.setUint16(entry + 18, answerLength, true);
      textCursor += answerLength;
      view.setInt32(entry + 20, card.dueOffset, true);
      view.setUint16(entry + 24, card.interval, true);
      view.setUint16(entry + 26, card.order, true);
      view.setUint8(entry + 28, card.kind);
    }
    return bytes;
  }

  function addSkipReasons(target, source) {
    for (const [reason, count] of source) {
      target.set(reason, (target.get(reason) || 0) + count);
    }
  }

  function buildTypeDecks(sourceFilename, noteTypes, cardsByType, mappings) {
    const decks = [];
    const skipped = new Map();
    const invalid = new Map();
    for (const mapping of mappings) {
      if (mapping.selected === false) {
        invalid.set(mapping.noteTypeId, 'Not selected for conversion.');
        continue;
      }
      const type = noteTypes.get(mapping.noteTypeId);
      if (!type) {
        invalid.set(mapping.noteTypeId, 'The note type metadata is unavailable.');
        continue;
      }
      const fields = Array.isArray(mapping.fields)
        ? mapping.fields.filter((field) =>
          field && Number.isInteger(field.index) && field.index >= 0 && field.index < type.fields.length &&
          (field.side === 'front' || field.side === 'back')
        )
        : [];
      if (!fields.some((field) => field.side === 'front') || !fields.some((field) => field.side === 'back')) {
        invalid.set(mapping.noteTypeId, 'Choose at least one Front and one Back field.');
        continue;
      }
      const resolved = resolveTypeCards(cardsByType.get(mapping.noteTypeId) || [], { ...mapping, fields });
      addSkipReasons(skipped, resolved.skipped);
      if (!resolved.cards.length) {
        invalid.set(mapping.noteTypeId, 'No cards remain with this field mapping.');
        continue;
      }
      const title = deckTitleForType(sourceFilename, type.name);
      decks.push({
        noteTypeId: mapping.noteTypeId,
        title,
        filename: outputName(mapping.filename),
        cards: resolved.cards,
        bytes: buildDeck(title, resolved.cards)
      });
    }
    return { decks, skipped, invalid };
  }

  window.AnkiConverter = {
    currentAnkiDay,
    collectionCreationTime,
    readNoteTypes,
    readEligibleCards,
    defaultDeckFilename,
    normalizeDecksForUpload,
    resolveTypeCards,
    buildTypeDecks,
    addSkipReasons,
    duplicateDestinationNames,
    destinationKey
  };
})();
