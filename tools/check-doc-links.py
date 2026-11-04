"""Check tracked Markdown's local inline links and image targets."""
import re
import subprocess
import sys
from html import unescape
from pathlib import Path
from urllib.parse import unquote, urlsplit

ROOT = Path(__file__).resolve().parents[1]


def targets(text):
    text = re.sub(r'```.*?```|~~~.*?~~~', '', text, flags=re.S)
    markdown = re.findall(r'!?\[[^\]\n]*\]\(([^)\s]+)(?:\s+"[^"]*")?\)', text)
    images = re.findall(r'<img\b[^>]*\bsrc=["\']([^"\']+)["\']', text, flags=re.I)
    return markdown + images


def missing_target(root, source, raw):
    url = urlsplit(unescape(raw.strip('<>')))
    if url.scheme or url.netloc or not url.path:
        return False
    relative = unquote(url.path)
    base = root if relative.startswith('/') else source.parent
    target = (base / relative.lstrip('/')).resolve()
    return not target.is_relative_to(root.resolve()) or not target.exists()


def main():
    if '--self-test' in sys.argv:
        import tempfile
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / 'README.md'
            (root / 'guide.md').write_text('# Guide', encoding='utf-8')
            assert targets('[Guide](guide.md#example)\n```\n[Ignore](absent.md)\n```') == ['guide.md#example']
            assert not missing_target(root, source, 'guide.md#example')
            assert not missing_target(root, source, 'https://example.com/absent')
            assert not missing_target(root, source, '#section')
            assert missing_target(root, source, 'absent.md')
            assert missing_target(root, source, '../private.md')
        print('Documentation link self-checks passed.')
        return 0
    files = subprocess.check_output(['git', 'ls-files', '-z'], cwd=ROOT).decode('utf-8').split('\0')
    errors = []
    for relative in files:
        if not relative.endswith('.md'):
            continue
        source = ROOT / relative
        for raw in targets(source.read_text(encoding='utf-8')):
            # shortcut: validate local targets, not fragments or external availability; extend when those need a gate.
            if missing_target(ROOT, source, raw):
                errors.append(f'{relative}: missing local target {raw}')
    if errors:
        print('\n'.join(errors), file=sys.stderr)
        return 1
    print('Tracked Markdown local links and images resolve.')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
