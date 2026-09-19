import unittest
from pathlib import Path
from xml.etree import ElementTree as ET

from test.web_portal.fixtures.epub_chapters import book

ROOT = Path(__file__).resolve().parents[2]

class EpubChaptersTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        module = ROOT / 'web/pages/epub-chapters.js'
        if not module.exists():
            raise AssertionError('EPUB chapter optimizer is missing')
        from playwright.sync_api import sync_playwright
        cls.pw = sync_playwright().start()
        cls.browser = cls.pw.chromium.launch(channel='chrome', headless=False)
        cls.page = cls.browser.new_page()
        cls.page.add_script_tag(path=str(ROOT / 'src/network/html/js/jszip.min.js'))
        cls.page.add_script_tag(path=str(module))

    @classmethod
    def tearDownClass(cls):
        if hasattr(cls, 'browser'): cls.browser.close()
        if hasattr(cls, 'pw'): cls.pw.stop()

    def optimize(self, files, cancel=False):
        return self.page.evaluate('''async ({files, cancel}) => {
          const zip = new JSZip();
          for (const [path, text] of Object.entries(files)) zip.file(path, text);
          const input = new File([await zip.generateAsync({type:'uint8array'})], 'test.epub');
          const output = await EpubChapters.optimize(input, {cancelled:()=>cancel});
          const result = await JSZip.loadAsync(output.file);
          const entries = {};
          for (const [path, entry] of Object.entries(result.files)) if (!entry.dir) entries[path] = await entry.async('string');
          return {changed:output.changed, same:output.file===input, entries};
        }''', {'files':files,'cancel':cancel})

    def test_policy_and_second_half_boundary(self):
        actual = self.page.evaluate('''() => ({
          markers: EpubChapters.findMarkers('<body><p>1</p><p>Text</p><div><p><span>２</span></p></div></body>').map(x=>x.title),
          ignored: EpubChapters.findMarkers('<body><h1>1 Start</h1><p>2 Next</p><p>1234</p></body>').length,
          reach: [[0,7],[0,8]].map(x=>EpubChapters.isWorkingToc(x,17))
        })''')
        self.assertEqual(actual, {'markers':['1','２'],'ignored':0,'reach':[False,True]})

    def test_split_preserves_nested_content_and_relocates_links(self):
        files = book({'part1.xhtml':'<section id="section"><p>Intro</p><p>1</p><p><ruby>猫<rt>ねこ</rt></ruby><a id="ref" href="#note">Note</a></p><div><p><span>２</span></p><p id="note">Footnote<a href="#ref">Return</a></p></div></section>',
                      'other.xhtml':'<p><a href="part1.xhtml#note">External</a></p>'})
        result = self.optimize(files)
        self.assertTrue(result['changed'])
        entries = result['entries']
        self.assertIn('OPS/part1_mr2.xhtml', entries)
        self.assertIn('part1_mr2.xhtml#note', entries['OPS/other.xhtml'])
        self.assertIn('part1_mr2.xhtml#note', entries['OPS/part1.xhtml'])
        self.assertIn('part1.xhtml#ref', entries['OPS/part1_mr2.xhtml'])
        self.assertIn('Intro', entries['OPS/part1.xhtml'])
        self.assertNotIn('Footnote', entries['OPS/part1.xhtml'])
        self.assertIn('Footnote', entries['OPS/part1_mr2.xhtml'])
        for path,text in entries.items():
            if path.endswith(('.xhtml','.opf','.ncx')):
                root=ET.fromstring(text)
                ids=[e.attrib['id'] for e in root.iter() if 'id' in e.attrib]
                self.assertEqual(len(ids),len(set(ids)),path)
        self.assertIn('part1_mr2.xhtml', entries['OPS/nav.xhtml'])
        self.assertIn('part1_mr2.xhtml', entries['OPS/toc.ncx'])

    def test_headings_and_single_marker_are_unchanged(self):
        for body in ['<h1>1 Introduction</h1><h1>2 Next</h1>', '<p>1</p><p>Only one</p>']:
            with self.subTest(body=body):
                result=self.optimize(book({'novel.xhtml':body}))
                self.assertFalse(result['changed'])
                self.assertTrue(result['same'])

    def test_stub_recovery_and_working_toc(self):
        bodies={f'part{i}.xhtml':f'<h1>Chapter {i}</h1><p>Text</p>' for i in range(1,5)}
        for nav,ncx in [(True,False),(False,True),(True,True)]:
            with self.subTest(nav=nav,ncx=ncx):
                result=self.optimize(book(bodies,nav,ncx))
                self.assertTrue(result['changed'])
                key='OPS/nav.xhtml' if nav else 'OPS/toc.ncx'
                self.assertIn('Chapter 4',result['entries'][key])
                result=self.optimize(book(bodies,nav,ncx,links=['part1.xhtml','part4.xhtml']))
                self.assertFalse(result['changed'])

    def test_kanji_markers_and_existing_filename_collision(self):
        result=self.optimize(book({'novel.xhtml':'<p>一</p><p>First</p><p>二</p><p>Second</p>',
                                   'novel_mr2.xhtml':'<p>Do not overwrite</p>'}))
        self.assertIn('Do not overwrite',result['entries']['OPS/novel_mr2.xhtml'])
        self.assertIn('Second',result['entries']['OPS/novel_mr3.xhtml'])

    def test_cancel_and_malformed_source_do_not_return_partial_output(self):
        files=book({'novel.xhtml':'<p>1</p><p>2</p>'})
        with self.assertRaisesRegex(Exception,'AbortError'):
            self.optimize(files,True)
        files['OPS/novel.xhtml']='<html><body><p>1</p><p>2</body>'
        with self.assertRaisesRegex(Exception,'XML'):
            self.optimize(files)

    def test_middle_chapter_keeps_styled_ancestors(self):
        result = self.optimize(book({'novel.xhtml':'<section class="vertical"><div class="indent"><p>1</p><p>First</p><p>2</p><p><ruby>猫<rt>ねこ</rt></ruby></p><p>3</p><p>Last</p></div></section>'}))
        middle = ET.fromstring(result['entries']['OPS/novel_mr2.xhtml'])
        ns = {'x':'http://www.w3.org/1999/xhtml'}
        self.assertIsNotNone(middle.find('.//x:section[@class="vertical"]/x:div[@class="indent"]/x:p/x:ruby', ns))
        self.assertNotIn('First', result['entries']['OPS/novel_mr2.xhtml'])
        self.assertNotIn('Last', result['entries']['OPS/novel_mr2.xhtml'])

    def test_upload_preparation_and_transport_fallback(self):
        source = (ROOT / 'web/pages/files.js').read_text()
        upload = source[source.index('async function uploadFile()'):source.index('function showFailedUploadsBanner()')]
        for scenario in ['disabled', 'converted', 'conversion_failure', 'invalid', 'cancel', 'stale', 'http']:
            with self.subTest(scenario=scenario):
                result = self.page.evaluate('''async ({source, scenario}) => {
                  document.body.innerHTML = '<input id="fileInput" type="file"><input id="convertBeforeUpload" type="checkbox"><input id="renameFromMetadataToggle" type="checkbox"><div id="uploadModalClose"></div><div id="progress-container"></div><div id="progress-fill"></div><div id="progress-text"></div><button id="uploadBtn"></button>';
                  const picked = new File(['original'], 'test.epub');
                  const dt = new DataTransfer(); dt.items.add(picked); document.getElementById('fileInput').files = dt.files;
                  document.getElementById('convertBeforeUpload').checked = ['converted','conversion_failure','invalid'].includes(scenario);
                  const run = new Function('scenario', `return (async () => {
                    let isUploadInProgress=false, uploadGeneration=0, operationCancelled=false;
                    const exportLogCheckbox=null, failedUploadsGlobal=[];
                    const calls=[], logs=[];
                    const setTimeout=()=>0;
                    const fetchExistingUploadNames=async()=>new Set();
                    const reserveAvailableUploadFilename=name=>name;
                    const log=(...args)=>logs.push(args.join(' ')), logError=log;
                    const showLog=()=>{}, clearLog=()=>{};
                    const restoreAfterCancel=()=>calls.push('cancelled');
                    const EpubChapters={optimize:async(file,options)=>{
                      calls.push('optimize');
                      if(scenario==='cancel') operationCancelled=true;
                      if(scenario==='stale') uploadGeneration++;
                      if(options.cancelled()) throw new DOMException('cancel','AbortError');
                      if(scenario==='invalid') throw new Error('invalid XML');
                      return {file:new File(['chapters'],file.name),changed:true};
                    }};
                    const convertEpubFile=async(file)=>{
                      calls.push('convert:'+await file.text());
                      if(scenario==='conversion_failure') throw new Error('image failed');
                      return new Blob(['converted']);
                    };
                    const uploadFileWebSocket=async(file)=>{
                      calls.push('ws:'+await file.text());
                      if(scenario==='http') throw new Error('WebSocket connection failed');
                    };
                    const uploadFileHTTP=async(file)=>calls.push('http:'+await file.text());
                    ${source}
                    await uploadFile();
                    for(let i=0;i<100;i++) await new Promise(requestAnimationFrame);
                    return {calls,logs};
                  })()`);
                  return await run(scenario);
                }''', {'source':upload, 'scenario':scenario})
                expected = {
                    'disabled':['optimize','ws:chapters'],
                    'converted':['optimize','convert:chapters','ws:converted'],
                    'conversion_failure':['optimize','convert:chapters','ws:chapters'],
                    'invalid':['optimize','ws:original'],
                    'cancel':['optimize','cancelled'],
                    'stale':['optimize'],
                    'http':['optimize','ws:chapters','http:chapters'],
                }
                self.assertEqual(result['calls'], expected[scenario])
                if scenario == 'invalid': self.assertTrue(any('optimization skipped' in line for line in result['logs']))

    def test_split_keeps_unrelated_nested_navigation(self):
        files = book({'novel.xhtml':'<p>1</p><p>First</p><p>2</p><p>Second</p>', 'appendix.xhtml':'<p>Appendix</p>'})
        files['OPS/nav.xhtml'] = files['OPS/nav.xhtml'].replace('</a></li>', '</a><ol><li><a href="appendix.xhtml">Appendix</a></li></ol></li>')
        result = self.optimize(files)
        nav = ET.fromstring(result['entries']['OPS/nav.xhtml'])
        ns = {'x':'http://www.w3.org/1999/xhtml'}
        self.assertIsNotNone(nav.find('.//x:ol/x:li/x:ol/x:li/x:a[@href="appendix.xhtml"]', ns))
        self.assertIn('novel_mr2.xhtml', result['entries']['OPS/nav.xhtml'])

    def test_no_toc_split_and_nav_precedence(self):
        result = self.optimize(book({'novel.xhtml':'<p>1</p><p>2</p>'}, nav=False, ncx=False))
        self.assertTrue(result['changed'])
        bodies = {f'part{i}.xhtml':f'<p>Part {i}</p>' for i in range(1,5)}
        self.assertFalse(self.optimize(book(bodies,False,False))['changed'])
        files = book(bodies, links=['part1.xhtml','part4.xhtml'])
        files['OPS/toc.ncx'] = book(bodies)['OPS/toc.ncx']
        self.assertFalse(self.optimize(files)['changed'])
        files['OPS/nav.xhtml'] = book(bodies)['OPS/nav.xhtml']
        files['OPS/toc.ncx'] = book(bodies,links=['part1.xhtml','part4.xhtml'])['OPS/toc.ncx']
        self.assertTrue(self.optimize(files)['changed'])

    def test_invalid_ids_and_xml_base_abort(self):
        for body in ['<p id="same">1</p><p id="same">2</p>', '<div xml:base="other/"><p>1</p><p>2</p></div>']:
            with self.subTest(body=body), self.assertRaises(Exception):
                self.optimize(book({'novel.xhtml':body}))

    def test_spine_ids_are_not_duplicated_and_split_group_recovers_missing_item(self):
        files = book({'part1.xhtml':'<p>1</p><p>2</p>', 'part2.xhtml':'<h1>Final chapter</h1><p>Text</p>'})
        files['OPS/book.opf'] = files['OPS/book.opf'].replace('idref="c0"', 'id="original-ref" idref="c0"')
        result = self.optimize(files)
        self.assertEqual(result['entries']['OPS/book.opf'].count('id="original-ref"'), 1)
        self.assertIn('Final chapter', result['entries']['OPS/nav.xhtml'])

    def test_encoded_filename_and_image_survive(self):
        files = book({'chapter #1.xhtml':'<p>1</p><p><a href="#note">Note</a></p><p>2</p><p id="note"><img src="pic.svg"/>End</p>'})
        for path in ['OPS/book.opf','OPS/nav.xhtml','OPS/toc.ncx']:
            files[path] = files[path].replace('chapter #1.xhtml', 'chapter%20%231.xhtml')
        files['OPS/pic.svg'] = '<svg xmlns="http://www.w3.org/2000/svg"><circle r="2"/></svg>'
        result = self.optimize(files)
        self.assertIn('chapter%20%231_mr2.xhtml#note', result['entries']['OPS/chapter #1.xhtml'])
        self.assertIn('pic.svg', result['entries']['OPS/chapter #1_mr2.xhtml'])
        self.assertIn('OPS/pic.svg', result['entries'])
