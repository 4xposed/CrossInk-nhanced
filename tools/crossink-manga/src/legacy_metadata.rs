use crate::input::resolve_epub_path as resolve;
use anyhow::{Context, Result};
use std::{fs::File, io::Read, path::Path, process::Command};

#[derive(Debug, Default, PartialEq, Eq)]
pub struct BookMetadata {
    pub title: String,
    pub author: String,
    pub language: String,
    /// Zero-based indices in the EPUB's original spine order, before page filtering.
    pub chapters: Vec<(usize, String)>,
}

pub fn read(input: &Path) -> Result<BookMetadata> {
    if input.is_dir() {
        return Ok(BookMetadata::default());
    }
    match input
        .extension()
        .and_then(|s| s.to_str())
        .unwrap_or("")
        .to_ascii_lowercase()
        .as_str()
    {
        "epub" => read_epub(input),
        "zip" | "cbz" => read_comicinfo(input),
        "pdf" => read_pdf(input),
        _ => Ok(BookMetadata::default()),
    }
}

fn xml_entry(archive: &mut zip::ZipArchive<File>, name: &str) -> Result<String> {
    let entry = archive
        .by_name(name)
        .with_context(|| format!("metadata member {name}"))?;
    const LIMIT: u64 = 8 * 1024 * 1024;
    anyhow::ensure!(entry.size() <= LIMIT, "metadata XML exceeds 8 MiB: {name}");
    let mut text = String::new();
    entry.take(LIMIT + 1).read_to_string(&mut text)?;
    anyhow::ensure!(
        text.len() as u64 <= LIMIT,
        "metadata XML exceeds 8 MiB: {name}"
    );
    Ok(text)
}

fn node_text(node: roxmltree::Node<'_, '_>) -> String {
    node.descendants()
        .filter(|n| n.is_text())
        .filter_map(|n| n.text())
        .collect::<String>()
        .trim()
        .to_owned()
}

fn first_text(root: roxmltree::Node<'_, '_>, tag: &str) -> String {
    root.descendants()
        .find(|n| n.has_tag_name(tag))
        .map(node_text)
        .unwrap_or_default()
}

fn read_comicinfo(input: &Path) -> Result<BookMetadata> {
    let mut archive = zip::ZipArchive::new(File::open(input)?)?;
    let name = archive
        .file_names()
        .find(|name| {
            name.rsplit('/')
                .next()
                .is_some_and(|name| name.eq_ignore_ascii_case("comicinfo.xml"))
        })
        .map(str::to_owned);
    let Some(name) = name else {
        return Ok(BookMetadata::default());
    };
    let text = xml_entry(&mut archive, &name)?;
    let doc = roxmltree::Document::parse(&text).context("parse ComicInfo metadata")?;
    Ok(BookMetadata {
        title: first_text(doc.root(), "Title"),
        author: first_text(doc.root(), "Writer"),
        language: first_text(doc.root(), "LanguageISO"),
        chapters: Vec::new(),
    })
}

fn read_epub(input: &Path) -> Result<BookMetadata> {
    let mut archive = zip::ZipArchive::new(File::open(input)?)?;
    let container = xml_entry(&mut archive, "META-INF/container.xml")?;
    let doc = roxmltree::Document::parse(&container).context("parse EPUB container metadata")?;
    let opf = doc
        .descendants()
        .find(|n| n.has_tag_name("rootfile"))
        .and_then(|n| n.attribute("full-path"))
        .context("EPUB metadata rootfile missing")?;
    let opf = resolve("", opf)?;
    let package = xml_entry(&mut archive, &opf)?;
    let doc = roxmltree::Document::parse(&package).context("parse EPUB package metadata")?;
    let metadata = doc.descendants().find(|n| n.has_tag_name("metadata"));
    let mut result = BookMetadata::default();
    if let Some(metadata) = metadata {
        result.title = first_text(metadata, "title");
        result.author = first_text(metadata, "creator");
        result.language = first_text(metadata, "language");
    }
    match read_epub_navigation(&mut archive, &opf, &doc) {
        Ok(chapters) => result.chapters = chapters,
        Err(error) => eprintln!("Warning: EPUB navigation unavailable: {error:#}"),
    }
    Ok(result)
}

fn read_epub_navigation(
    archive: &mut zip::ZipArchive<File>,
    opf: &str,
    doc: &roxmltree::Document<'_>,
) -> Result<Vec<(usize, String)>> {
    let items: Vec<_> = doc
        .descendants()
        .filter(|n| n.has_tag_name("item"))
        .collect();
    let mut spine_paths = std::collections::HashMap::new();
    for (index, spine) in doc
        .descendants()
        .filter(|n| n.has_tag_name("itemref"))
        .enumerate()
    {
        if let Some(href) = items
            .iter()
            .find(|n| n.attribute("id") == spine.attribute("idref"))
            .and_then(|n| n.attribute("href"))
        {
            spine_paths.entry(resolve(opf, href)?).or_insert(index);
        }
    }
    let nav = items
        .iter()
        .find(|n| {
            n.attribute("properties")
                .is_some_and(|p| p.split_whitespace().any(|p| p == "nav"))
        })
        .and_then(|n| n.attribute("href"));
    let ncx_id = doc
        .descendants()
        .find(|n| n.has_tag_name("spine"))
        .and_then(|n| n.attribute("toc"));
    let ncx = ncx_id
        .and_then(|id| items.iter().find(|n| n.attribute("id") == Some(id)))
        .and_then(|n| n.attribute("href"));
    let Some(href) = nav.or(ncx) else {
        return Ok(Vec::new());
    };
    let nav_path = resolve(opf, href)?;
    let text = xml_entry(archive, &nav_path)?;
    let doc = roxmltree::Document::parse(&text).context("parse EPUB navigation metadata")?;
    let entries: Vec<_> = if nav.is_some() {
        doc.descendants()
            .filter(|n| {
                n.has_tag_name("nav")
                    && n.attributes().any(|a| {
                        a.name() == "type" && a.value().split_whitespace().any(|v| v == "toc")
                    })
            })
            .flat_map(|n| n.descendants())
            .filter(|n| n.has_tag_name("a"))
            .filter_map(|n| Some((n.attribute("href")?, node_text(n))))
            .collect()
    } else {
        doc.descendants()
            .filter(|n| n.has_tag_name("navPoint"))
            .filter_map(|n| {
                let href = n
                    .children()
                    .find(|n| n.has_tag_name("content"))?
                    .attribute("src")?;
                let label = n.children().find(|n| n.has_tag_name("navLabel"))?;
                Some((href, node_text(label)))
            })
            .collect()
    };
    let mut chapters = Vec::new();
    for (href, title) in entries {
        if title.is_empty() {
            continue;
        }
        if let Ok(path) = resolve(&nav_path, href)
            && let Some(&index) = spine_paths.get(&path)
        {
            chapters.push((index, title));
        }
    }
    Ok(chapters)
}

fn read_pdf(input: &Path) -> Result<BookMetadata> {
    let input = input.canonicalize().context("resolve PDF metadata path")?;
    let output = Command::new("pdfinfo")
        .args(["-enc", "UTF-8"])
        .env("LC_ALL", "C")
        .arg(input)
        .output()
        .context("PDF metadata requires native Poppler pdfinfo")?;
    anyhow::ensure!(
        output.status.success(),
        "PDF metadata inspection failed: {}",
        String::from_utf8_lossy(&output.stderr)
    );
    let text = String::from_utf8(output.stdout).context("PDF metadata output is not UTF-8")?;
    let value = |field: &str| {
        text.lines()
            .find_map(|line| line.strip_prefix(field))
            .unwrap_or("")
            .trim()
            .to_owned()
    };
    Ok(BookMetadata {
        title: value("Title:"),
        author: value("Author:"),
        ..BookMetadata::default()
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Write;

    fn archive(path: &Path, entries: &[(&str, &str)]) {
        let mut archive = zip::ZipWriter::new(File::create(path).unwrap());
        for (name, text) in entries {
            archive
                .start_file(*name, zip::write::SimpleFileOptions::default())
                .unwrap();
            archive.write_all(text.as_bytes()).unwrap();
        }
        archive.finish().unwrap();
    }

    #[test]
    fn comicinfo_decodes_entities_and_language() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("book.cbz");
        archive(
            &path,
            &[(
                "chapter/ComicInfo.xml",
                "<ComicInfo><Title>A &amp; B</Title><Writer>猫</Writer><LanguageISO>ja</LanguageISO></ComicInfo>",
            )],
        );
        assert_eq!(
            read(&path).unwrap(),
            BookMetadata {
                title: "A & B".into(),
                author: "猫".into(),
                language: "ja".into(),
                chapters: vec![]
            }
        );
    }

    const CONTAINER: &str =
        "<container><rootfiles><rootfile full-path='OPS/book.opf'/></rootfiles></container>";
    const OPF: &str = r#"<package xmlns:dc="http://purl.org/dc/elements/1.1/"><metadata><dc:title>A &amp; B</dc:title><dc:creator>猫</dc:creator><dc:language>ja</dc:language></metadata><manifest><item id="b" href="b.xhtml"/><item id="a" href="a.xhtml"/><item id="nav" href="nav/toc.xhtml" properties="nav"/></manifest><spine><itemref idref="b"/><itemref idref="a"/></spine></package>"#;

    #[test]
    fn epub_navigation_uses_wrapper_spine_order_and_flattens_titles() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("book.epub");
        archive(
            &path,
            &[
                ("META-INF/container.xml", CONTAINER),
                ("OPS/book.opf", OPF),
                (
                    "OPS/nav/toc.xhtml",
                    r##"<html xmlns:epub="http://www.idpf.org/2007/ops"><nav epub:type="landmarks"><a href="../b.xhtml">Ignore</a></nav><nav epub:type="toc"><a href="../a.xhtml#section">猫 <span>&amp; B</span></a><a href="../absent.xhtml">Missing</a></nav></html>"##,
                ),
            ],
        );
        assert_eq!(
            read(&path).unwrap(),
            BookMetadata {
                title: "A & B".into(),
                author: "猫".into(),
                language: "ja".into(),
                chapters: vec![(1, "猫 & B".into())]
            }
        );
    }

    #[test]
    fn encoded_package_navigation_and_spine_paths_resolve_to_archive_members() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("encoded.epub");
        let container = CONTAINER.replace("OPS/book.opf", "Book%20Files/book%20one.opf");
        let opf = OPF
            .replace("a.xhtml", "chapter%20%E7%8C%AB.xhtml")
            .replace("nav/toc.xhtml", "nav/toc%20one.xhtml");
        archive(
            &path,
            &[
                ("META-INF/container.xml", &container),
                ("Book Files/book one.opf", &opf),
                (
                    "Book Files/nav/toc one.xhtml",
                    r##"<html xmlns:epub="http://www.idpf.org/2007/ops"><nav epub:type="toc"><a href="../chapter%20%E7%8C%AB.xhtml#section">Encoded chapter</a></nav></html>"##,
                ),
            ],
        );
        assert_eq!(
            read(&path).unwrap(),
            BookMetadata {
                title: "A & B".into(),
                author: "猫".into(),
                language: "ja".into(),
                chapters: vec![(1, "Encoded chapter".into())],
            }
        );
    }

    #[test]
    fn optional_missing_or_malformed_navigation_keeps_book_metadata() {
        let ncx_opf = OPF
            .replace(" properties=\"nav\"", "")
            .replace("<spine>", "<spine toc=\"nav\">");
        for opf in [OPF, ncx_opf.as_str()] {
            for navigation in [None, Some("<broken nav")] {
                let dir = tempfile::tempdir().unwrap();
                let path = dir.path().join("broken-nav.epub");
                let mut entries =
                    vec![("META-INF/container.xml", CONTAINER), ("OPS/book.opf", opf)];
                if let Some(text) = navigation {
                    entries.push(("OPS/nav/toc.xhtml", text));
                }
                archive(&path, &entries);
                assert_eq!(
                    read(&path).unwrap(),
                    BookMetadata {
                        title: "A & B".into(),
                        author: "猫".into(),
                        language: "ja".into(),
                        chapters: vec![],
                    }
                );
            }
        }
    }

    #[test]
    fn epub2_nested_ncx_maps_every_chapter() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("book.epub");
        let opf = OPF
            .replace(
                "href=\"nav/toc.xhtml\" properties=\"nav\"",
                "href=\"nav/toc.ncx\"",
            )
            .replace("<spine>", "<spine toc=\"nav\">");
        archive(
            &path,
            &[
                ("META-INF/container.xml", CONTAINER),
                ("OPS/book.opf", &opf),
                (
                    "OPS/nav/toc.ncx",
                    r##"<ncx><navMap><navPoint><navLabel><text>First</text></navLabel><content src="../b.xhtml"/><navPoint><navLabel><text>Second &amp; Last</text></navLabel><content src="../a.xhtml#x"/></navPoint></navPoint></navMap></ncx>"##,
                ),
            ],
        );
        assert_eq!(
            read(&path).unwrap().chapters,
            vec![(0, "First".into()), (1, "Second & Last".into())]
        );
    }
}
