"""Small synthetic EPUB archives for browser chapter-rewrite tests."""


def book(bodies, nav=True, ncx=True, links=None):
    paths = list(bodies)
    links = links if links is not None else [paths[0]]
    files = {'mimetype': 'application/epub+zip',
             'META-INF/container.xml': '<container xmlns="urn:oasis:names:tc:opendocument:xmlns:container"><rootfiles><rootfile full-path="OPS/book.opf"/></rootfiles></container>'}
    items = ''.join(f'<item id="c{i}" href="{p}" media-type="application/xhtml+xml"/>' for i, p in enumerate(paths))
    spine = ''.join(f'<itemref idref="c{i}"/>' for i in range(len(paths)))
    if nav:
        items += '<item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/>'
        files['OPS/nav.xhtml'] = '<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops"><body><nav epub:type="toc"><ol>' + ''.join(f'<li><a href="{p}">Original</a></li>' for p in links) + '</ol></nav></body></html>'
    if ncx:
        items += '<item id="ncx" href="toc.ncx" media-type="application/x-dtbncx+xml"/>'
        files['OPS/toc.ncx'] = '<ncx xmlns="http://www.daisy.org/z3986/2005/ncx/"><navMap>' + ''.join(f'<navPoint id="n{i}" playOrder="{i+1}"><navLabel><text>Original</text></navLabel><content src="{p}"/></navPoint>' for i, p in enumerate(links)) + '</navMap></ncx>'
    files['OPS/book.opf'] = f'<package xmlns="http://www.idpf.org/2007/opf" version="3.0"><manifest>{items}</manifest><spine>{spine}</spine></package>'
    for path, body in bodies.items():
        files['OPS/' + path] = '<html xmlns="http://www.w3.org/1999/xhtml"><head><title>Fixture</title><link href="style.css" rel="stylesheet"/></head><body>' + body + '</body></html>'
    files['OPS/style.css'] = 'p { color: black; }'
    return files
