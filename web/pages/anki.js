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
  const $ = (id) => document.getElementById(id);
  const encoder = new TextEncoder();
  const state = { busy: false, converted: null };
  const { currentAnkiDay, collectionCreationTime, readNoteTypes, readEligibleCards, defaultDeckFilename,
    normalizeDecksForUpload, buildTypeDecks, addSkipReasons, duplicateDestinationNames, destinationKey } =
    window.AnkiConverter;

  function status(message, kind) {
    const el = $('conversionStatus');
    el.hidden = false;
    el.className = `status ${kind || ''}`;
    el.textContent = message;
  }

  function setBusy(busy) {
    state.busy = busy;
    $('apkgFile').disabled = busy;
    for (const control of document.querySelectorAll('.anki-note-type input, .anki-note-type select')) {
      control.disabled = busy;
    }
    $('uploadDeck').disabled = busy || !state.converted || !state.converted.decks.length;
  }

  function clearConvertedDeck() {
    state.converted = null;
    $('noteTypeMappings').replaceChildren();
    $('noteTypeMappingSection').hidden = true;
    $('deckOutputs').replaceChildren();
    $('deckOutputs').hidden = true;
    $('uploadDeck').disabled = true;
    $('sourceSummary').hidden = true;
    $('skipReasons').hidden = true;
    $('skipReasons').replaceChildren();
  }
  function showConversion(filename, decks, skipped) {
    const cardCount = decks.reduce((total, deck) => total + deck.cards.length, 0);
    $('sourceSummary').hidden = false;
    $('sourceSummary').textContent = `${filename}: ${cardCount} card${cardCount === 1 ? '' : 's'} accepted in ${decks.length} deck${decks.length === 1 ? '' : 's'}.`;
    const list = $('skipReasons');
    list.replaceChildren();
    for (const [reason, count] of skipped) {
      const item = document.createElement('li');
      item.textContent = `${count} ${reason}${count === 1 ? '' : 's'} skipped`;
      list.appendChild(item);
    }
    list.hidden = skipped.size === 0;
  }

  function updatePrimaryControl(row) {
    const role = row.querySelector('.anki-field-role');
    const primary = row.querySelector('.anki-field-primary');
    primary.disabled = role.value !== 'front' && role.value !== 'back';
  }

  function renderMappings(sourceFilename, noteTypes) {
    const root = $('noteTypeMappings');
    const template = $('mappingTemplate');
    const fieldTemplate = $('fieldMappingRowTemplate');
    root.replaceChildren();
    for (const noteType of noteTypes) {
      const fragment = template.content.cloneNode(true);
      const panel = fragment.querySelector('.anki-note-type');
      const enabled = fragment.querySelector('.anki-note-type-enabled');
      const name = fragment.querySelector('.anki-note-type-name');
      const fieldRows = fragment.querySelector('.anki-field-rows');
      const deckName = fragment.querySelector('.anki-deck-name');
      panel.dataset.noteTypeId = noteType.id;
      enabled.checked = true;
      name.textContent = noteType.name;
      for (let index = 0; index < noteType.fields.length; index += 1) {
        const fieldFragment = fieldTemplate.content.cloneNode(true);
        const row = fieldFragment.querySelector('.anki-field-row');
        const fieldName = fieldFragment.querySelector('.anki-field-name');
        const role = fieldFragment.querySelector('.anki-field-role');
        row.dataset.fieldIndex = String(index);
        fieldName.textContent = noteType.fields[index];
        role.value = index === 0 ? 'front' : index === 1 ? 'back' : '';
        role.addEventListener('change', () => {
          updatePrimaryControl(row);
          rebuildDecks();
        });
        fieldFragment.querySelector('.anki-field-primary').addEventListener('change', rebuildDecks);
        updatePrimaryControl(row);
        fieldRows.appendChild(fieldFragment);
      }
      deckName.value = defaultDeckFilename(sourceFilename, noteType.name);
      enabled.addEventListener('change', rebuildDecks);
      deckName.addEventListener('change', rebuildDecks);
      root.appendChild(fragment);
    }
    $('noteTypeMappingSection').hidden = false;
  }

  function selectedFieldMappings(panel) {
    return Array.from(panel.querySelectorAll('.anki-field-row'), (row) => {
      const index = Number(row.dataset.fieldIndex);
      const side = row.querySelector('.anki-field-role').value;
      if (!Number.isSafeInteger(index) || (side !== 'front' && side !== 'back')) return null;
      return {
        index,
        side,
        primary: row.querySelector('.anki-field-primary').checked
      };
    }).filter(Boolean);
  }

  function rebuildDecks() {
    if (!state.converted || state.busy) return;
    const panels = new Map();
    const mappings = [];
    for (const panel of document.querySelectorAll('.anki-note-type')) {
      const noteTypeId = panel.dataset.noteTypeId;
      panels.set(noteTypeId, {
        panel,
        status: panel.querySelector('.anki-type-status')
      });
      mappings.push({
        noteTypeId,
        selected: panel.querySelector('.anki-note-type-enabled').checked,
        fields: selectedFieldMappings(panel),
        filename: panel.querySelector('.anki-deck-name').value
      });
    }
    const converted = buildTypeDecks(
      state.converted.sourceFilename,
      state.converted.noteTypes,
      state.converted.cardsByType,
      mappings
    );
    const skipped = new Map(state.converted.skipped);
    addSkipReasons(skipped, converted.skipped);
    const outputs = $('deckOutputs');
    outputs.replaceChildren();
    for (const [noteTypeId, message] of converted.invalid) {
      panels.get(noteTypeId).status.textContent = message;
    }
    const duplicateNames = duplicateDestinationNames(converted.decks);
    const decks = [];
    for (const deck of converted.decks) {
      const panel = panels.get(deck.noteTypeId);
      if (duplicateNames.has(destinationKey(deck.filename))) {
        panel.status.textContent = 'Choose a distinct deck filename for this note type.';
        continue;
      }
      panel.status.textContent = `${deck.cards.length} card${deck.cards.length === 1 ? '' : 's'} ready for ${DECK_DIRECTORY}/${deck.filename}.`;
      const item = document.createElement('li');
      item.textContent = `${DECK_DIRECTORY}/${deck.filename}: ${deck.cards.length} card${deck.cards.length === 1 ? '' : 's'}.`;
      outputs.appendChild(item);
      decks.push({ ...deck, panel: panel.panel });
    }
    outputs.hidden = decks.length === 0;
    state.converted.decks = duplicateNames.size ? [] : decks;
    showConversion(state.converted.sourceFilename, duplicateNames.size ? converted.decks : decks, skipped);
    $('uploadDeck').disabled = duplicateNames.size || !decks.length;
  }

  async function loadSql() {
    if (typeof window.initSqlJs !== 'function') {
      throw new Error('SQL.js could not be loaded. Connect this browser to the internet and try again.');
    }
    try {
      return await window.initSqlJs({ locateFile: () => SQLJS_WASM_URL });
    } catch (_) {
      throw new Error('SQL.js could not be loaded. Connect this browser to the internet and try again.');
    }
  }

  async function loadCollection(file) {
    const SQL = await loadSql();
    if (!window.JSZip) throw new Error('Could not load the package reader. Refresh the page and try again.');
    let zip;
    try {
      zip = await window.JSZip.loadAsync(file);
    } catch (_) {
      throw new Error('Could not open the .apkg archive. It may be password-protected or corrupt.');
    }
    const collection = zip.file('collection.anki21') || zip.file('collection.anki2');
    if (!collection) {
      if (zip.file('collection.anki21b')) {
        throw new Error('This package uses collection.anki21b. Export it from Anki Desktop as a SQLite-compatible package, then try again.');
      }
      throw new Error('This package contains no readable Anki SQLite collection.');
    }
    let db;
    try {
      const bytes = await collection.async('uint8array');
      db = new SQL.Database(bytes);
      const today = currentAnkiDay(collectionCreationTime(db));
      const noteTypeMetadata = readNoteTypes(db);
      const { cards, skipped } = readEligibleCards(db, today, noteTypeMetadata);
      const cardsByType = new Map();
      for (const card of cards) {
        const typeCards = cardsByType.get(card.noteTypeId) || [];
        typeCards.push(card);
        cardsByType.set(card.noteTypeId, typeCards);
      }
      const noteTypes = Array.from(cardsByType.keys())
        .map((id) => noteTypeMetadata.get(id))
        .sort((left, right) => left.name.localeCompare(right.name) || left.id.localeCompare(right.id));
      return { cards, skipped, cardsByType, noteTypes };
    } catch (error) {
      if (error && error.message && (error.message.startsWith('This package') || error.message.startsWith('The Anki collection has'))) throw error;
      throw new Error('The Anki collection is missing or unreadable.');
    } finally {
      if (db) db.close();
    }
  }

  function temporaryName() {
    const bytes = new Uint8Array(8);
    crypto.getRandomValues(bytes);
    return `crossink-upload-${Array.from(bytes, (byte) => byte.toString(16).padStart(2, '0')).join('')}.tmp`;
  }

  async function ensureDeckDirectory() {
    const body = new FormData();
    body.append('path', '/');
    body.append('name', 'decks');
    const response = await fetch('/mkdir', { method: 'POST', body });
    if (response.ok) return;
    const detail = await response.text();
    if (response.status === 400 && detail === 'Folder already exists') return;
    throw new Error(detail || 'Could not create the decks folder.');
  }

  async function removeTemporary(path) {
    const response = await fetch('/delete', {
      method: 'POST',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body: `path=${encodeURIComponent(path)}`
    });
    if (!response.ok) throw new Error((await response.text()) || 'Could not clean up the temporary upload.');
  }

  async function uploadAtomic(bytes, filename) {
    const temporary = temporaryName();
    const temporaryPath = `${DECK_DIRECTORY}/${temporary}`;
    try {
      const form = new FormData();
      form.append('file', new Blob([bytes], { type: 'application/octet-stream' }), temporary);
      const upload = await fetch(`/upload?path=${encodeURIComponent(DECK_DIRECTORY)}`, { method: 'POST', body: form });
      if (!upload.ok) throw new Error((await upload.text()) || 'Upload failed.');
      const rename = new FormData();
      rename.append('path', temporaryPath);
      rename.append('name', filename);
      const finalized = await fetch('/rename', { method: 'POST', body: rename });
      if (!finalized.ok) {
        if (finalized.status === 409) throw new Error('The destination already exists; choose a different deck name.');
        throw new Error((await finalized.text()) || 'Could not finalize upload.');
      }
    } catch (error) {
      try {
        await removeTemporary(temporaryPath);
      } catch (cleanupError) {
        throw new Error(`${error.message || String(error)} Temporary cleanup failed: ${cleanupError.message || String(cleanupError)}`);
      }
      throw error;
    }
  }

  async function convertSelectedFile() {
    const file = $('apkgFile').files[0];
    clearConvertedDeck();
    if (!file) return;
    if (!/\.apkg$/i.test(file.name)) {
      status('Choose an Anki package with the .apkg extension.', 'error');
      return;
    }
    setBusy(true);
    try {
      status('Reading Anki package…');
      const { cards, skipped, cardsByType, noteTypes } = await loadCollection(file);
      if (!cards.length || !noteTypes.length) throw new Error('No eligible cards were found. Nothing was uploaded.');
      state.converted = {
        sourceFilename: file.name,
        skipped,
        cardsByType,
        noteTypes: new Map(noteTypes.map((noteType) => [noteType.id, noteType])),
        decks: []
      };
      renderMappings(file.name, noteTypes);
    } catch (error) {
      status(error.message || String(error), 'error');
    } finally {
      setBusy(false);
      if (state.converted) {
        rebuildDecks();
        status(
          state.converted.decks.length
            ? 'Review each note type field mapping, then upload the converted decks.'
            : 'Select compatible note types and at least one Front and Back field before uploading.',
          state.converted.decks.length ? 'success' : 'error'
        );
      }
    }
  }

  async function uploadDecksSequentially(decks, upload, onProgress) {
    let uploaded = 0;
    try {
      for (const deck of decks) {
        onProgress(uploaded, decks.length, deck);
        await upload(deck);
        uploaded += 1;
      }
      return uploaded;
    } catch (error) {
      const failure = error instanceof Error ? error : new Error(String(error));
      failure.uploaded = uploaded;
      throw failure;
    }
  }
  async function uploadDecksToDeckDirectory(decks, onProgress) {
    const normalizedDecks = normalizeDecksForUpload(decks);
    await ensureDeckDirectory();
    return uploadDecksSequentially(
      normalizedDecks,
      (deck) => uploadAtomic(deck.bytes, deck.filename),
      onProgress
    );
  }


  async function uploadConvertedDeck() {
    if (state.busy || !state.converted || !state.converted.decks.length) return;
    setBusy(true);
    try {
      const uploaded = await uploadDecksToDeckDirectory(
        state.converted.decks,
        (completed, total, deck) => status(`Uploading ${completed + 1} of ${total}: ${DECK_DIRECTORY}/${deck.filename}…`)
      );
      const finalPaths = state.converted.decks.map((deck) => `${DECK_DIRECTORY}/${deck.filename}`);
      for (const deck of state.converted.decks) {
        deck.panel.querySelector('.anki-note-type-enabled').checked = false;
      }
      clearConvertedDeck();
      status(`Uploaded ${uploaded} deck${uploaded === 1 ? '' : 's'}: ${finalPaths.join(', ')}.`, 'success');
    } catch (error) {
      const uploaded = error && Number.isSafeInteger(error.uploaded) ? error.uploaded : 0;
      status(
        `${uploaded ? `Uploaded ${uploaded} deck${uploaded === 1 ? '' : 's'}. ` : ''}${error.message || String(error)} Fix the conflicting deck filename and continue.`,
        'error'
      );
    } finally {
      setBusy(false);
      if (state.converted) rebuildDecks();
    }
  }

  if (window.__ANKI_PORTAL_TEST_MODE__) {
    window.__ankiPortalTest = { ...window.AnkiConverter, uploadDecksSequentially, uploadDecksToDeckDirectory };
  }

  function start() {
    $('apkgFile').addEventListener('change', () => { void convertSelectedFile(); });
    $('uploadDeck').addEventListener('click', () => { void uploadConvertedDeck(); });
  }

  start();
})();
