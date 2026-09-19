/* Browser-only EPUB chapter preparation. Input archives remain untouched on failure. */
(() => {
  'use strict';
  const XHTML = 'http://www.w3.org/1999/xhtml';
  const EPUB = 'http://www.idpf.org/2007/ops';
  const NUMBER = '(?:[0-9]{1,3}|[０-９]{1,3}|[〇一二三四五六七八九十百]{1,4})';
  function findMarkers(html) {
    const re = new RegExp('(?:<div[^>]*>\\s*)?<p[^>]*>\\s*(?:<span[^>]*>\\s*)?(' + NUMBER + ')\\s*(?:<\\/span>\\s*)?<\\/p>\\s*(?:<\\/div>)?', 'gs');
    const found = [];
    for (const match of html.matchAll(re)) {
      const before = html.slice(0, match.index).trimEnd();
      if (!before || before.endsWith('>')) found.push({offset: match.index, title: match[1]});
    }
    return found;
  }
  function structuralKey(href) {
    const match = href.match(/^(.*?)(\d+)(\.(?:xhtml|html|htm))$/i);
    return match ? match[1] + '#' + match[3].toLowerCase() : null;
  }
  function chapterTitle(html, fallback) {
    const body = (html.match(/<body\b[^>]*>([\s\S]*?)<\/body>/i) || [])[1] || html;
    const textOf = value => {
      const doc = new DOMParser().parseFromString(value.replace(/<rt\b[^>]*>[\s\S]*?<\/rt>/gi, ''), 'text/html');
      return doc.body.textContent.trim();
    };
    const heading = body.slice(0, 5000).match(/<h[1-6]\b[^>]*>[\s\S]*?<\/h[1-6]>/i);
    if (heading) return textOf(heading[0]) || fallback;
    const first = (body.match(/<p\b[^>]*>[\s\S]*?<\/p>/gi) || []).map(textOf).find(Boolean);
    return first && first.length <= 80 ? first : fallback;
  }
  function isWorkingToc(indices, spineCount) {
    const refs = [...new Set(indices)].filter(i => i >= 0 && i < spineCount);
    return refs.length >= 2 && Math.max(...refs) >= (spineCount - 1) / 2;
  }
  const elements = (node, name) => [...node.getElementsByTagNameNS('*', name)];
  const serialize = doc => new XMLSerializer().serializeToString(doc);
  function parse(text, path) {
    const doc = new DOMParser().parseFromString(text, 'application/xml');
    if (doc.getElementsByTagName('parsererror').length || !doc.documentElement) throw new Error(`Invalid XML in ${path}`);
    if ([...doc.getElementsByTagName('*')].some(node => node.hasAttributeNS('http://www.w3.org/XML/1998/namespace', 'base'))) {
      throw new Error(`Unsupported XML base in ${path}`);
    }
    return doc;
  }
  function resolve(from, href) {
    const url = new URL(href, 'https://epub.invalid/' + from.split('/').map(encodeURIComponent).join('/'));
    if (url.origin !== 'https://epub.invalid') return null;
    return {path: decodeURIComponent(url.pathname.slice(1)), fragment: decodeURIComponent(url.hash.slice(1)), search: url.search};
  }
  function relative(from, to) {
    const base = from.split('/'); base.pop();
    const target = to.split('/');
    while (base.length && target.length && base[0] === target[0]) { base.shift(); target.shift(); }
    return '../'.repeat(base.length) + target.map(encodeURIComponent).join('/');
  }
  function checkIds(doc, path) {
    const ids = new Set();
    for (const node of doc.getElementsByTagName('*')) {
      const id = node.getAttribute('id') || node.getAttributeNS('http://www.w3.org/XML/1998/namespace', 'id');
      if (id && ids.has(id)) throw new Error(`Duplicate ID in ${path}: ${id}`);
      if (id) ids.add(id);
    }
    return ids;
  }
  async function optimize(file, options = {}) {
    const checkCancelled = () => { if (options.cancelled?.()) throw new DOMException('Upload cancelled', 'AbortError'); };
    checkCancelled();
    const zip = await JSZip.loadAsync(file);
    checkCancelled();
    const read = async path => {
      if (!zip.file(path)) throw new Error(`Missing EPUB entry: ${path}`);
      const text = await zip.file(path).async('string'); checkCancelled(); return text;
    };
    const container = parse(await read('META-INF/container.xml'), 'container.xml');
    const opfPath = elements(container, 'rootfile')[0]?.getAttribute('full-path');
    if (!opfPath) throw new Error('Missing EPUB package path');
    const opf = parse(await read(opfPath), opfPath);
    const manifestNode = elements(opf, 'manifest')[0], spineNode = elements(opf, 'spine')[0];
    if (!manifestNode || !spineNode) throw new Error('Missing EPUB manifest or spine');
    const manifest = new Map();
    let navPath, ncxPath;
    for (const item of elements(manifestNode, 'item')) {
      const target = resolve(opfPath, item.getAttribute('href') || '');
      if (!target) continue;
      manifest.set(item.getAttribute('id'), {path:target.path, item});
      if ((item.getAttribute('properties') || '').split(/\s+/).includes('nav')) navPath = target.path;
      if (item.getAttribute('media-type') === 'application/x-dtbncx+xml') ncxPath = target.path;
    }
    const spine = elements(spineNode, 'itemref').map(ref => {
      const entry = manifest.get(ref.getAttribute('idref'));
      if (!entry) throw new Error('Invalid EPUB spine reference');
      return {...entry, ref};
    });
    const navText = navPath ? await read(navPath) : '';
    const ncxText = ncxPath ? await read(ncxPath) : '';
    const documents = new Map();
    if (navPath) documents.set(navPath, parse(navText, navPath));
    if (ncxPath) documents.set(ncxPath, parse(ncxText, ncxPath));
    const names = new Set([...((navText || ncxText).matchAll(/(?:href|src)="([^"]+)"/g))].map(m => m[1].split('#')[0].split('/').pop()));
    const referenced = spine.map((entry, i) => names.has(entry.path.split('/').pop()) ? i : -1).filter(i => i >= 0);
    const structuralCounts = new Map();
    for (const entry of spine) {
      const key = structuralKey(entry.path);
      if (key) structuralCounts.set(key, (structuralCounts.get(key) || 0) + 1);
    }
    const additions = [], recoverable = [], relocation = new Map(), order = new Map(), origins = new Map();
    const usedPaths = new Set(Object.keys(zip.files)), usedIds = new Set(manifest.keys());
    let changed = false;
    for (let position = 0; position < spine.length; ++position) {
      checkCancelled();
      const entry = spine[position]; order.set(entry.path, position);
      if (!/\.(xhtml|html|htm)$/i.test(entry.path) || entry.path === navPath) continue;
      const text = await read(entry.path), doc = parse(text, entry.path);
      checkIds(doc, entry.path);
      documents.set(entry.path, doc);
      const markers = findMarkers(text);
      if (markers.length < 2) {
        if ((structuralCounts.get(structuralKey(entry.path)) || 0) >= 2) {
          recoverable.push({path:entry.path, title:chapterTitle(text, entry.path.split('/').pop().replace(/\.[^.]+$/, ''))});
        }
        continue;
      }
      const body = elements(doc, 'body')[0];
      if (!body) throw new Error('Missing EPUB body');
      const paragraphOffsets = [...text.matchAll(/<p\b[^>]*>/g)].map(m => m.index);
      const paragraphs = elements(doc, 'p');
      if (paragraphOffsets.length !== paragraphs.length) throw new Error('Ambiguous EPUB paragraph mapping');
      const boundaries = markers.map(marker => {
        const index = paragraphOffsets.findIndex(offset => offset >= marker.offset);
        const paragraph = paragraphs[index];
        if (!paragraph || !body.contains(paragraph) || paragraph.textContent.trim() !== marker.title) {
          throw new Error('Ambiguous EPUB chapter boundary');
        }
        return paragraph;
      });
      // Range cloning preserves partially selected ancestor blocks and all child content.
      // Reopened ancestor IDs stay on their first occurrence across the split parts.
      const seenIds = new Set();
      let lastRef = entry.ref;
      for (let part = 0; part < markers.length; ++part) {
        const output = doc.cloneNode(true), outBody = elements(output, 'body')[0];
        const outParagraphs = elements(output, 'p');
        // Trim a full clone, rather than copying a Range's common-ancestor
        // contents: middle chapters must retain every styling ancestor too.
        if (part + 1 < boundaries.length) {
          const tail = output.createRange();
          tail.setStartBefore(outParagraphs[paragraphs.indexOf(boundaries[part + 1])]);
          tail.setEnd(outBody, outBody.childNodes.length);
          tail.deleteContents();
        }
        if (part > 0) {
          const head = output.createRange();
          head.setStart(outBody, 0);
          head.setEndBefore(outParagraphs[paragraphs.indexOf(boundaries[part])]);
          head.deleteContents();
        }
        let path = entry.path;
        if (part > 0) {
          const dot = entry.path.lastIndexOf('.');
          let suffix = part + 1, id;
          do { path = entry.path.slice(0,dot) + '_mr' + suffix + entry.path.slice(dot); id = entry.item.getAttribute('id') + '-mr' + suffix++; }
          while (usedPaths.has(path) || usedIds.has(id));
          usedPaths.add(path); usedIds.add(id);
          const item = entry.item.cloneNode(true);
          item.setAttribute('id', id); item.setAttribute('href', relative(opfPath,path));
          manifestNode.appendChild(item);
          const ref = entry.ref.cloneNode(true); ref.setAttribute('idref',id);
          ref.removeAttribute('id'); ref.removeAttributeNS('http://www.w3.org/XML/1998/namespace','id');
          lastRef.after(ref); lastRef = ref;
        }
        for (const node of output.getElementsByTagName('*')) {
          const id = node.getAttribute('id') || node.getAttributeNS('http://www.w3.org/XML/1998/namespace','id');
          if (!id) continue;
          if (seenIds.has(id)) { node.removeAttribute('id'); node.removeAttributeNS('http://www.w3.org/XML/1998/namespace','id'); }
          else { seenIds.add(id); relocation.set(entry.path + '#' + id, path); }
        }
        checkIds(output,path);
        documents.set(path,output); order.set(path, position + part / markers.length); origins.set(path,entry.path);
        additions.push({path,title:markers[part].title});
      }
      changed = true;
    }
    if ((navText || ncxText) && !isWorkingToc(referenced, spine.length) && recoverable.length > 0 &&
        recoverable.some(entry => !names.has(entry.path.split('/').pop()))) {
      additions.push(...recoverable); changed = true;
    }
    if (!changed) return {file, changed:false, chapters:0};
    additions.sort((a,b) => order.get(a.path) - order.get(b.path));
    // Other retained XML resources can contain references into moved chapters.
    for (const path of Object.keys(zip.files)) {
      if (!documents.has(path) && /\.(xhtml|html|htm|svg)$/i.test(path) && !zip.files[path].dir) {
        documents.set(path,parse(await read(path),path));
      }
    }
    for (const [path,doc] of documents) {
      for (const node of doc.getElementsByTagName('*')) {
        for (const attr of [...node.attributes]) {
          if (!['href','src'].includes(attr.localName)) continue;
          const target = resolve(path,attr.value);
          // Continuation parts inherit links relative to the original document.
          let origin = path;
          for (const entry of spine) {
            if (path !== entry.path && order.has(path) && Math.floor(order.get(path)) === order.get(entry.path)) { origin=entry.path; break; }
          }
          const original = resolve(origin,attr.value);
          if (!target || !original) continue;
          const moved = relocation.get(original.path + '#' + original.fragment);
          if (moved) attr.value = relative(path,moved) + original.search + '#' + encodeURIComponent(original.fragment);
        }
      }
    }
    function rebuildToc(path, kind) {
      if (!path) return;
      const doc = documents.get(path), ns = kind === 'nav' ? XHTML : doc.documentElement.namespaceURI;
      const nav = kind === 'nav' ? elements(doc,'nav').find(n => (n.getAttributeNS(EPUB,'type') || n.getAttribute('epub:type') || '').split(/\s+/).includes('toc')) : elements(doc,'navMap')[0];
      if (!nav) throw new Error('Missing EPUB table of contents');
      const list = kind === 'nav' ? elements(nav,'ol')[0] : nav;
      if (!list) throw new Error('Missing EPUB navigation list');
      const affected = new Set(additions.map(a=>a.path));
      const linkName = kind === 'nav' ? 'a' : 'content', attr = kind === 'nav' ? 'href' : 'src';
      const retained = new Map();
      for (const link of elements(list,linkName)) {
        const target = resolve(path,link.getAttribute(attr) || '');
        if (!target || !affected.has(target.path)) continue;
        const item = kind === 'nav' ? link.closest('li') : link.parentNode;
        if (!item?.parentNode) continue;
        if (!retained.has(target.path)) {
          const entry = additions.find(a=>a.path===target.path);
          link.setAttribute(attr,relative(path,entry.path));
          if (kind === 'nav') link.textContent = entry.title;
          else {
            const label = [...item.children].find(n=>n.localName==='navLabel');
            const text = label && elements(label,'text')[0];
            if (!text) throw new Error('Missing EPUB navigation label');
            text.textContent = entry.title;
          }
          retained.set(target.path,item);
          continue;
        }
        const children = kind === 'nav' ? [...item.children].filter(n=>n.localName==='ol').flatMap(n=>[...n.children]) : [...item.children].filter(n=>n.localName==='navPoint');
        for (const child of children) item.before(child);
        item.remove();
      }
      const existingIds = checkIds(doc,path);
      for (let i=0;i<additions.length;++i) {
        const entry=additions[i];
        if (retained.has(entry.path)) continue;
        const item=doc.createElementNS(ns,kind==='nav'?'li':'navPoint');
        if (kind==='nav') {
          const link=doc.createElementNS(ns,'a'); link.setAttribute('href',relative(path,entry.path)); link.textContent=entry.title; item.appendChild(link);
        } else {
          let ordinal=i+1, id;
          do { id='crossink-chapter-'+ordinal++; } while (existingIds.has(id));
          existingIds.add(id); item.setAttribute('id',id);
          const label=doc.createElementNS(ns,'navLabel'), text=doc.createElementNS(ns,'text'); text.textContent=entry.title; label.appendChild(text);
          const content=doc.createElementNS(ns,'content'); content.setAttribute('src',relative(path,entry.path)); item.append(label,content);
        }
        const after=[...list.children].find(child=>{
          const link=elements(child,linkName)[0];
          const target=link && resolve(path,link.getAttribute(attr)||'');
          return target && (order.get(target.path) ?? Infinity) > order.get(entry.path);
        });
        const previous = i > 0 ? additions[i-1] : null;
        const sibling = previous && origins.has(entry.path) && origins.get(previous.path) === origins.get(entry.path)
          ? retained.get(previous.path) : null;
        if (sibling) sibling.after(item);
        else list.insertBefore(item,after || null);
        retained.set(entry.path,item);
      }
      if (kind==='ncx') elements(nav,'navPoint').forEach((n,i)=>n.setAttribute('playOrder',i+1));
    }
    rebuildToc(navPath,'nav'); rebuildToc(ncxPath,'ncx');
    documents.set(opfPath,opf);
    const output = new JSZip();
    output.file('mimetype','application/epub+zip',{compression:'STORE',createFolders:false});
    for (const [path,entry] of Object.entries(zip.files)) {
      checkCancelled();
      if (entry.dir || path==='mimetype' || documents.has(path)) continue;
      output.file(path,await entry.async('uint8array'),{createFolders:false});
    }
    for (const [path,doc] of documents) {
      const text=serialize(doc); parse(text,path); checkIds(doc,path);
      output.file(path,text,{createFolders:false});
    }
    // Verify rewritten fragment links into known XML documents and all spine paths.
    for (const [path,doc] of documents) {
      for (const node of doc.getElementsByTagName('*')) {
        for (const attr of [...node.attributes]) {
          if (!['href','src'].includes(attr.localName)) continue;
          const target=resolve(path,attr.value);
          if (target?.fragment && [...relocation.values()].includes(target.path) &&
              documents.has(target.path) && !checkIds(documents.get(target.path),target.path).has(target.fragment)) {
            throw new Error('Unresolved EPUB fragment after splitting');
          }
        }
      }
    }
    for (const ref of elements(spineNode,'itemref')) {
      const item=elements(manifestNode,'item').find(n=>n.getAttribute('id')===ref.getAttribute('idref'));
      const target=item && resolve(opfPath,item.getAttribute('href'));
      if (!target || !output.file(target.path)) throw new Error('Unresolved EPUB spine after splitting');
    }
    checkCancelled();
    const bytes=await output.generateAsync({type:'uint8array',compression:'DEFLATE'});
    checkCancelled();
    return {file:new File([bytes],file.name,{type:'application/epub+zip',lastModified:file.lastModified}),changed:true,chapters:additions.length};
  }
  window.EpubChapters={findMarkers,structuralKey,chapterTitle,isWorkingToc,optimize};
})();
