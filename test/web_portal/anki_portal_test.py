import json
import runpy
import subprocess
import textwrap
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


class AnkiPortalTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.preview = runpy.run_path(str(ROOT / "scripts/preview_web.py"))

    def run_converter(self, expression):
        harness = textwrap.dedent(
            f"""
            const fs = require('fs');
            const vm = require('vm');
            const source = ['anki-converter.js', 'anki.js'].map((filename) => fs.readFileSync({json.dumps(str(ROOT / 'web/pages'))} + '/' + filename, 'utf8')).join('\\n');
            const elements = new Map();
            const element = () => ({{
              addEventListener() {{}},
              appendChild() {{}},
              querySelectorAll() {{ return []; }},
              replaceChildren() {{}},
              disabled: false,
              files: [],
              hidden: true,
              textContent: '',
              value: ''
            }});
            global.Node = {{ TEXT_NODE: 3, ELEMENT_NODE: 1 }};
            global.DOMParser = class {{
              parseFromString(value) {{
                return {{ body: {{ childNodes: [{{ nodeType: Node.TEXT_NODE, nodeValue: String(value) }}] }} }};
              }}
            }};
            global.document = {{
              createElement: element,
              getElementById(id) {{
                if (!elements.has(id)) elements.set(id, element());
                return elements.get(id);
              }},
              querySelectorAll() {{ return []; }}
            }};
            global.__ANKI_PORTAL_TEST_MODE__ = true;
            global.window = global;
            Object.defineProperty(global, 'crypto', {{ value: {{ getRandomValues(words) {{ words.fill(1); return words; }} }}, configurable: true }});
            vm.runInThisContext(source, {{ filename: 'anki.js' }});
            const api = window.__ankiPortalTest;
            if (!api) throw new Error('Anki converter test API is unavailable');
            const result = {expression};
            Promise.resolve(result).then((value) => process.stdout.write(JSON.stringify(value)));
            """
        )
        completed = subprocess.run(
            ["node", "-e", harness],
            check=True,
            capture_output=True,
            cwd=ROOT,
            text=True,
        )
        return json.loads(completed.stdout)

    def test_converter_parses_note_type_metadata(self):
        result = self.run_converter(
            """(() => {
                const db = {
                  prepare() {
                    return {
                      step: () => true,
                      getAsObject: () => ({models: JSON.stringify({
                        '100': {id: 100, name: 'Basic', flds: [{name: 'Front'}, {name: 'Back'}]},
                        '200': {id: 200, name: 'Vocabulary', flds: [{name: 'Word'}, {name: 'Definition'}, {name: 'Hint'}]}
                      })}),
                      free() {}
                    };
                  }
                };
                return Array.from(api.readNoteTypes(db).values());
              })()"""
        )

        self.assertEqual(
            result,
            [
                {"id": "100", "name": "Basic", "fields": ["Front", "Back"]},
                {"id": "200", "name": "Vocabulary", "fields": ["Word", "Definition", "Hint"]},
            ],
        )

    def test_converter_preserves_ordered_field_blocks_and_promotes_a_duplicate_primary(self):
        result = self.run_converter(
            """(() => {
                const cards = [{
                  sourceCardId: 1n,
                  fields: [' Term \\r\\n', 'definition', 'Term', 'example'],
                  order: 0,
                  dueOffset: 0,
                  interval: 1,
                  kind: 0
                }];
                const resolved = api.resolveTypeCards(cards, {
                  fields: [
                    {index: 0, side: 'front', primary: false},
                    {index: 1, side: 'back', primary: false},
                    {index: 2, side: 'front', primary: true},
                    {index: 3, side: 'back', primary: true}
                  ],
                });
                const decoder = new TextDecoder();
                const decode = (blocks) => (blocks || []).map((block) => ({
                  text: decoder.decode(block.bytes),
                  primary: block.primary
                }));
                return {
                  prompt: decode(resolved.cards[0].promptBlocks),
                  answer: decode(resolved.cards[0].answerBlocks)
                };
              })()"""
        )

        self.assertEqual(
            result,
            {
                "prompt": [{"text": "Term", "primary": True}],
                "answer": [
                    {"text": "definition", "primary": False},
                    {"text": "example", "primary": True},
                ],
            },
        )

    def test_converter_serializes_a_deduplicated_sole_front_block_as_primary_without_selection(self):
        result = self.run_converter(
            """(() => {
                const bytes = api.buildTypeDecks(
                  'Study.apkg',
                  new Map([['1', {id: '1', name: 'Basic', fields: ['Front', 'Back', 'Duplicate']}]]),
                  new Map([['1', [{
                    sourceCardId: 7n,
                    fields: ['Term', 'Definition', ' Term \\r\\n'],
                    order: 0,
                    dueOffset: 0,
                    interval: 1,
                    kind: 0
                  }]]]),
                  [{
                    noteTypeId: '1',
                    fields: [
                      {index: 0, side: 'front', primary: false},
                      {index: 2, side: 'front', primary: false},
                      {index: 1, side: 'back', primary: false}
                    ],
                    filename: 'Study.cdeck'
                  }]
                ).decks[0].bytes;
                const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
                const decoder = new TextDecoder();
                const readSide = (start) => {
                  let cursor = start;
                  const count = bytes[cursor++];
                  const blocks = [];
                  for (let index = 0; index < count; index += 1) {
                    const primary = bytes[cursor++];
                    const length = view.getUint16(cursor, true);
                    cursor += 2;
                    blocks.push({
                      primary,
                      text: decoder.decode(bytes.subarray(cursor, cursor + length))
                    });
                    cursor += length;
                  }
                  return {blocks, length: cursor - start};
                };
                const prompt = readSide(view.getUint32(28, true));
                return {
                  prompt: prompt.blocks,
                  answer: readSide(view.getUint32(28, true) + prompt.length).blocks
                };
              })()"""
        )

        self.assertEqual(
            result,
            {
                "prompt": [{"primary": 1, "text": "Term"}],
                "answer": [{"primary": 0, "text": "Definition"}],
            },
        )

    def test_converter_serializes_a_later_duplicate_primary_on_its_first_field_block(self):
        result = self.run_converter(
            """(() => {
                const bytes = api.buildTypeDecks(
                  'Study.apkg',
                  new Map([['1', {id: '1', name: 'Basic', fields: ['Front', 'Back', 'Duplicate', 'Example']}]]),
                  new Map([['1', [{
                    sourceCardId: 7n,
                    fields: ['Term', 'Definition', ' Term \\r\\n', 'Example'],
                    order: 0,
                    dueOffset: 0,
                    interval: 1,
                    kind: 0
                  }]]]),
                  [{
                    noteTypeId: '1',
                    fields: [
                      {index: 0, side: 'front', primary: false},
                      {index: 2, side: 'front', primary: true},
                      {index: 3, side: 'front', primary: false},
                      {index: 1, side: 'back', primary: false}
                    ],
                    filename: 'Study.cdeck'
                  }]
                ).decks[0].bytes;
                const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
                const decoder = new TextDecoder();
                const readSide = (start) => {
                  let cursor = start;
                  const count = bytes[cursor++];
                  const blocks = [];
                  for (let index = 0; index < count; index += 1) {
                    const primary = bytes[cursor++];
                    const length = view.getUint16(cursor, true);
                    cursor += 2;
                    blocks.push({
                      primary,
                      text: decoder.decode(bytes.subarray(cursor, cursor + length))
                    });
                    cursor += length;
                  }
                  return {blocks, length: cursor - start};
                };
                const prompt = readSide(view.getUint32(28, true));
                return {
                  prompt: prompt.blocks,
                  answer: readSide(view.getUint32(28, true) + prompt.length).blocks
                };
              })()"""
        )

        self.assertEqual(
            result,
            {
                "prompt": [
                    {"primary": 1, "text": "Term"},
                    {"primary": 0, "text": "Example"},
                ],
                "answer": [{"primary": 0, "text": "Definition"}],
            },
        )

    def test_converter_writes_v2_field_payload_primary_flags_and_index_record_size(self):
        result = self.run_converter(
            """(() => {
                const bytes = api.buildTypeDecks(
                  'Study.apkg',
                  new Map([['1', {id: '1', name: 'Basic', fields: ['Front', 'Back', 'Hint']}]]),
                  new Map([['1', [{
                    sourceCardId: 7n,
                    fields: ['Front', 'Back', 'Hint'],
                    order: 0,
                    dueOffset: 0,
                    interval: 1,
                    kind: 0
                  }]]]),
                  [{
                    noteTypeId: '1',
                    fields: [
                      {index: 0, side: 'front', primary: true},
                      {index: 1, side: 'back', primary: false},
                      {index: 2, side: 'back', primary: true}
                    ],
                    filename: 'Study.cdeck'
                  }]
                ).decks[0].bytes;
                return Array.from(bytes);
              })()"""
        )

        self.assertEqual(result, list((ROOT / "test" / "anki_deck" / "fixtures" / "portal-v2.cdeck").read_bytes()))

        self.assertEqual(result[4:8], [2, 0, 32, 0])
        self.assertEqual(result[20:22], [13, 0])
        self.assertEqual(result[22:24], [29, 0])
        self.assertEqual(result[24:32], [45, 0, 0, 0, 74, 0, 0, 0])
        self.assertEqual(result[74:], [
            1, 1, 5, 0, 70, 114, 111, 110, 116,
            2, 0, 4, 0, 66, 97, 99, 107, 1, 4, 0, 72, 105, 110, 116,
        ])

    def test_converter_rejects_field_block_limits_after_same_side_deduplication(self):
        result = self.run_converter(
            """(() => {
                const eight = Array.from({length: 8}, (_, index) => ({
                  index,
                  side: 'front',
                  primary: false
                }));
                const cards = [
                  {
                    sourceCardId: 1n,
                    fields: Array.from({length: 9}, (_, index) => `Field ${index}`),
                    order: 0,
                    dueOffset: 0,
                    interval: 1,
                    kind: 0
                  },
                  {
                    sourceCardId: 2n,
                    fields: ['x'.repeat(2049), 'Answer'],
                    order: 0,
                    dueOffset: 0,
                    interval: 1,
                    kind: 0
                  }
                ];
                const resolved = api.resolveTypeCards(cards, {
                  fields: [...eight, {index: 8, side: 'front', primary: true}, {index: 1, side: 'back', primary: false}],
                });
                return {cardCount: resolved.cards.length, skipped: Array.from(resolved.skipped.entries())};
              })()"""
        )

        self.assertEqual(result["cardCount"], 0)
        self.assertIn(["more than 8 field blocks on one side", 1], result["skipped"])
        self.assertIn(["field text longer than 2,048 bytes", 1], result["skipped"])

    def test_converter_concatenates_selected_fields_in_order_and_reverses_ordinal_one(self):
        result = self.run_converter(
            """(() => {
                const cards = [
                  {sourceCardId: 1n, fields: ['Front', 'Back', 'Extra'], order: 0, dueOffset: 0, interval: 1, kind: 0},
                  {sourceCardId: 2n, fields: ['Front two', 'Back two', 'Extra two'], order: 1, dueOffset: 0, interval: 1, kind: 0}
                ];
                const resolved = api.resolveTypeCards(cards, {
                  fields: [
                    {index: 2, side: 'front', primary: false},
                    {index: 0, side: 'front', primary: false},
                    {index: 1, side: 'back', primary: false}
                  ]
                });
                const decoder = new TextDecoder();
                const decode = (blocks) => blocks.map((block) => decoder.decode(block.bytes));
                return resolved.cards.map((card) => ({
                  prompt: decode(card.promptBlocks),
                  answer: decode(card.answerBlocks)
                }));
              })()"""
        )

        self.assertEqual(
            result,
            [
                {"prompt": ["Extra", "Front"], "answer": ["Back"]},
                {"prompt": ["Back two"], "answer": ["Extra two", "Front two"]},
            ],
        )
    def test_converter_deduplicates_normalized_values_per_side_before_limits_and_reverses_ordinal_one(self):
        result = self.run_converter(
            """(() => {
                const maximum = 'x'.repeat(2048);
                const cards = [
                  {sourceCardId: 1n, fields: [maximum, 'Answer', `${maximum}\\r\\n`], order: 0, dueOffset: 0, interval: 1, kind: 0},
                  {sourceCardId: 2n, fields: [' First \\r\\n', 'First', 'Second'], order: 0, dueOffset: 0, interval: 1, kind: 0},
                  {sourceCardId: 3n, fields: [' First reversed \\r\\n', 'Answer reversed', 'Second reversed'], order: 1, dueOffset: 0, interval: 1, kind: 0}
                ];
                const resolved = api.resolveTypeCards(cards, {
                  fields: [
                    {index: 2, side: 'front', primary: false},
                    {index: 0, side: 'front', primary: false},
                    {index: 1, side: 'back', primary: false}
                  ]
                });
                const decoder = new TextDecoder();
                const decode = (blocks) => blocks.map((block) => decoder.decode(block.bytes));
                return resolved.cards.map((card) => ({
                  prompt: decode(card.promptBlocks),
                  answer: decode(card.answerBlocks)
                }));
              })()"""
        )

        self.assertEqual(
            result,
            [
                {"prompt": ["x" * 2048], "answer": ["Answer"]},
                {"prompt": ["Second", "First"], "answer": ["First"]},
                {"prompt": ["Answer reversed"], "answer": ["Second reversed", "First reversed"]},
            ],
        )


    def test_converter_skips_cards_that_cannot_fit_binary_schedule_fields(self):
        result = self.run_converter(
            """(() => {
                const cards = [
                  {sourceCardId: 1n, fields: ['Front', 'Back'], order: 0, dueOffset: 2147483648, interval: 1, kind: 0},
                  {sourceCardId: 2n, fields: ['Front', 'Back'], order: 0, dueOffset: 0, interval: 65536, kind: 1}
                ];
                const resolved = api.resolveTypeCards(cards, {
                  fields: [
                    {index: 0, side: 'front', primary: false},
                    {index: 1, side: 'back', primary: false}
                  ]
                });
                return {cardCount: resolved.cards.length, skipped: Array.from(resolved.skipped.entries())};
              })()"""
        )

        self.assertEqual(result["cardCount"], 0)
        self.assertIn(["due offset outside signed 32-bit range", 1], result["skipped"])
        self.assertIn(["interval outside unsigned 16-bit range", 1], result["skipped"])

    def test_converter_builds_one_named_output_per_type_and_enforces_each_card_limit(self):
        result = self.run_converter(
            """(() => {
                const basicCards = Array.from({length: 4097}, (_, index) => ({
                  sourceCardId: BigInt(index + 1),
                  fields: [`Front ${index}`, `Back ${index}`],
                  order: 0,
                  dueOffset: 0,
                  interval: 1,
                  kind: 0
                }));
                const vocabularyCards = [{
                  sourceCardId: 5000n,
                  fields: ['Word', 'Definition'],
                  order: 0,
                  dueOffset: 0,
                  interval: 1,
                  kind: 0
                }];
                const noteTypes = new Map([
                  ['1', {id: '1', name: 'Basic', fields: ['Front', 'Back']}],
                  ['2', {id: '2', name: 'Vocabulary', fields: ['Word', 'Definition']}]
                ]);
                const converted = api.buildTypeDecks('Study.apkg', noteTypes, new Map([
                  ['1', basicCards],
                  ['2', vocabularyCards]
                ]), [
                  {noteTypeId: '1', fields: [{index: 0, side: 'front', primary: false}, {index: 1, side: 'back', primary: false}], filename: 'Study - Basic.cdeck'},
                  {noteTypeId: '2', fields: [{index: 0, side: 'front', primary: false}, {index: 1, side: 'back', primary: false}], filename: 'Study - Vocabulary.cdeck'}
                ]);
                return {
                  decks: converted.decks.map((deck) => ({filename: deck.filename, cardCount: deck.cards.length})),
                  skipped: Array.from(converted.skipped.entries())
                };
              })()"""
        )

        self.assertEqual(
            result["decks"],
            [
                {"filename": "Study - Basic.cdeck", "cardCount": 4096},
                {"filename": "Study - Vocabulary.cdeck", "cardCount": 1},
            ],
        )
        self.assertIn(["deck limit of 4,096 cards", 1], result["skipped"])

    def test_converter_prefills_and_normalizes_an_edited_deck_filename_for_upload(self):
        result = self.run_converter(
            """(() => {
                const cards = [{
                  sourceCardId: 1n,
                  fields: ['Front', 'Back'],
                  order: 0,
                  dueOffset: 0,
                  interval: 1,
                  kind: 0
                }];
                const decks = api.buildTypeDecks(
                  'French / Level 1.apkg',
                  new Map([['1', {id: '1', name: 'Basic / reverse', fields: ['Front', 'Back']}]]),
                  new Map([['1', cards]]),
                  [{noteTypeId: '1', fields: [{index: 0, side: 'front', primary: false}, {index: 1, side: 'back', primary: false}], filename: '  French:/Level 1  .CDECK'}]
                ).decks;
                const calls = [];
                class TestFormData {
                  constructor() { this.fields = []; }
                  append(name, value, filename) { this.fields.push({name, value, filename: filename || null}); }
                }
                global.FormData = TestFormData;
                global.Blob = class { constructor() {} };
                global.fetch = async (url, options = {}) => {
                  calls.push({url, fields: options.body && options.body.fields || []});
                  return {ok: true, status: 200, text: async () => ''};
                };
                return api.uploadDecksToDeckDirectory(decks, () => {}).then(() => ({
                  defaultName: api.defaultDeckFilename('French / Level 1.apkg', 'Basic / reverse'),
                  filename: decks[0].filename,
                  renamedTo: calls.find((call) => call.url === '/rename').fields.find((field) => field.name === 'name').value
                }));
              })()"""
        )

        self.assertEqual(result["defaultName"], "French Level 1 - Basic reverse.cdeck")
        self.assertEqual(result["filename"], "French Level 1.cdeck")
        self.assertEqual(result["renamedTo"], "French Level 1.cdeck")

    def test_converter_rejects_duplicate_normalized_destinations_before_requests(self):
        result = self.run_converter(
            """(() => {
                const calls = [];
                class TestFormData {
                  constructor() { this.fields = []; }
                  append(name, value, filename) { this.fields.push({name, value, filename: filename || null}); }
                }
                global.FormData = TestFormData;
                global.Blob = class { constructor() {} };
                global.fetch = async (url) => {
                  calls.push(url);
                  return {ok: true, status: 200, text: async () => ''};
                };
                return api.uploadDecksToDeckDirectory(
                  [{filename: 'Same:/deck.cdeck', bytes: [1]}, {filename: 'Same deck.CDECK', bytes: [2]}],
                  () => {}
                ).then(
                  () => ({calls, error: null}),
                  (error) => ({calls, error: error.message})
                );
              })()"""
        )

        self.assertEqual(result["calls"], [])
        self.assertEqual(result["error"], "Choose distinct deck filenames before uploading.")

    def test_converter_stops_sequential_upload_after_failure(self):
        result = self.run_converter(
            """(() => {
                const calls = [];
                const progress = [];
                return api.uploadDecksSequentially(
                  [{filename: 'one.cdeck'}, {filename: 'two.cdeck'}, {filename: 'three.cdeck'}],
                  async (deck) => {
                    calls.push(deck.filename);
                    if (deck.filename === 'two.cdeck') throw new Error('destination conflict');
                  },
                  (uploaded, total, deck) => progress.push(`${uploaded + 1}/${total}:${deck.filename}`)
                ).then(
                  () => ({calls, progress, error: null}),
                  (error) => ({calls, progress, error: `${error.message}:${error.uploaded}`})
                );
              })()"""
        )

        self.assertEqual(result["calls"], ["one.cdeck", "two.cdeck"])
        self.assertEqual(result["progress"], ["1/3:one.cdeck", "2/3:two.cdeck"])
        self.assertEqual(result["error"], "destination conflict:1")

    def test_converter_uploads_each_deck_atomically_in_decks_directory(self):
        result = self.run_converter(
            """(() => {
                const calls = [];
                class TestFormData {
                  constructor() { this.fields = []; }
                  append(name, value, filename) {
                    this.fields.push({name, value: name === 'file' ? '[deck bytes]' : value, filename: filename || null});
                  }
                }
                const temporaryPattern = /crossink-upload-[0-9a-f]{16}\\.tmp/g;
                const normalize = (value) => typeof value === 'string'
                  ? value.replace(temporaryPattern, '<temporary>')
                  : value;
                const normalizedCalls = () => calls.map((call) => ({
                  ...call,
                  body: typeof call.body === 'string'
                    ? normalize(call.body)
                    : call.body.map((field) => ({
                      ...field,
                      value: normalize(field.value),
                      filename: normalize(field.filename)
                    }))
                }));
                global.FormData = TestFormData;
                global.Blob = class { constructor() {} };
                global.fetch = async (url, options = {}) => {
                  const rename = url === '/rename' && options.body.fields && options.body.fields.find((field) => field.name === 'name');
                  const status = rename && rename.value === 'two.cdeck' ? 409 : 200;
                  calls.push({
                    url,
                    method: options.method,
                    body: typeof options.body === 'string' ? options.body : options.body.fields
                  });
                  return {
                    ok: status >= 200 && status < 300,
                    status,
                    text: async () => status === 409 ? 'exists' : ''
                  };
                };
                return Promise.resolve().then(() => api.uploadDecksToDeckDirectory(
                  [{filename: 'one.cdeck', bytes: [1]}, {filename: 'two.cdeck', bytes: [2]}],
                  () => {}
                )).then(
                  () => ({calls: normalizedCalls(), error: null}),
                  (error) => ({calls: normalizedCalls(), error: `${error.message}:${error.uploaded}`})
                );
              })()"""
        )

        temporary = "<temporary>"
        self.assertEqual(
            result["calls"],
            [
                {
                    "url": "/mkdir",
                    "method": "POST",
                    "body": [
                        {"name": "path", "value": "/", "filename": None},
                        {"name": "name", "value": "decks", "filename": None},
                    ],
                },
                {
                    "url": "/upload?path=%2Fdecks",
                    "method": "POST",
                    "body": [{"name": "file", "value": "[deck bytes]", "filename": temporary}],
                },
                {
                    "url": "/rename",
                    "method": "POST",
                    "body": [
                        {"name": "path", "value": f"/decks/{temporary}", "filename": None},
                        {"name": "name", "value": "one.cdeck", "filename": None},
                    ],
                },
                {
                    "url": "/upload?path=%2Fdecks",
                    "method": "POST",
                    "body": [{"name": "file", "value": "[deck bytes]", "filename": temporary}],
                },
                {
                    "url": "/rename",
                    "method": "POST",
                    "body": [
                        {"name": "path", "value": f"/decks/{temporary}", "filename": None},
                        {"name": "name", "value": "two.cdeck", "filename": None},
                    ],
                },
                {
                    "url": "/delete",
                    "method": "POST",
                    "body": f"path=%2Fdecks%2F{temporary}",
                },
            ],
        )
        self.assertEqual(
            result["error"],
            "The destination already exists; choose a different deck name.:1",
        )

    def test_converter_exposes_helpers_needed_to_complete_conversion(self):
        result = self.run_converter(
            """(() => {
                if (typeof api.addSkipReasons !== 'function' || typeof api.duplicateDestinationNames !== 'function' ||
                    typeof api.destinationKey !== 'function') {
                  return {merged: false};
                }
                const skipped = new Map([['existing reason', 2]]);
                api.addSkipReasons(skipped, new Map([['existing reason', 1], ['new reason', 3]]));
                const duplicates = Array.from(api.duplicateDestinationNames([
                  {filename: 'Basic.cdeck'},
                  {filename: 'basic.CDECK'}
                ]));
                return {
                  merged: true,
                  reasons: Array.from(skipped.entries()),
                  duplicates,
                  destinationKey: api.destinationKey('Basic.CDECK')
                };
              })()"""
        )

        self.assertEqual(
            result,
            {
                "merged": True,
                "reasons": [["existing reason", 3], ["new reason", 3]],
                "duplicates": ["basic.cdeck"],
                "destinationKey": "basic.cdeck",
            },
        )

    def test_anki_page_exposes_local_conversion_surface(self):
        self.assertIn("anki", self.preview["PAGES"])

        html = self.preview["render_page"]("anki")
        self.assertIn('href="/anki" class="active"', html)
        self.assertIn(
            'src="https://cdnjs.cloudflare.com/ajax/libs/sql.js/1.14.2/sql-wasm.js"',
            html,
        )
        self.assertIn('id="apkgFile"', html)
        self.assertIn('id="conversionStatus"', html)
        self.assertIn('id="uploadDeck"', html)

    def test_anki_page_reads_collection_creation_time_for_review_due_offsets(self):
        html = self.preview["render_page"]("anki")
        self.assertIn("SELECT crt FROM col", html)
        self.assertIn("Math.floor((Date.now() / 1000 - crt) / 86400)", html)

    def test_anki_page_rejects_zero_source_card_ids(self):
        html = self.preview["render_page"]("anki")
        self.assertIn("if (sourceCardId <= 0n)", html)


    def test_anki_page_exposes_per_note_type_field_mapping_surface(self):
        html = self.preview["render_page"]("anki")

        self.assertIn('id="noteTypeMappings"', html)
        self.assertIn('id="mappingTemplate"', html)
        self.assertIn('id="fieldMappingRowTemplate"', html)
        self.assertIn('class="anki-field-role"', html)
        self.assertIn('class="anki-field-primary" type="checkbox"', html)
        self.assertIn('class="anki-deck-name"', html)
        self.assertIn('id="deckOutputs"', html)

    def test_anki_page_groups_cards_by_note_type_and_resolves_selected_fields(self):
        html = self.preview["render_page"]("anki")

        self.assertIn("SELECT models FROM col", html)
        self.assertIn("notes.mid AS noteTypeId", html)
        self.assertIn("function resolveSideFieldBlocks(fields, mappings, side)", html)
        self.assertIn("function resolveTypeCards(eligibleCards, mapping)", html)
        self.assertIn("const promptBlocks = card.order === 0 ? front.blocks : back.blocks", html)
        self.assertIn("view.setUint16(4, 2, true)", html)
        self.assertIn("function deckTitleForType(sourceTitle, noteTypeName)", html)
        self.assertIn("const decks = []", html)

    def test_anki_page_uploads_each_converted_note_type_deck_in_sequence(self):
        html = self.preview["render_page"]("anki")
        self.assertIn("for (const deck of state.converted.decks)", html)
        self.assertIn("function uploadDecksSequentially(decks, upload, onProgress)", html)
        self.assertIn("Uploading ${completed + 1} of ${total}", html)

if __name__ == "__main__":
    unittest.main()
