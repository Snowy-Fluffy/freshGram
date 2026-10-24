#!/usr/bin/env python3
# Checks the built-in freshGram translations against lang.strings:
# unknown keys, placeholders, **markup**, line breaks and plural forms.
import os
import re
import sys

here = os.path.dirname(os.path.abspath(__file__))
line_re = re.compile(r'^"([A-Za-z0-9_#]+)"\s*=\s*"(.*)";$')
tag_re = re.compile(r'\{([a-z_0-9]+)\}')
prefixes = ('fresh_', 'ayu_', 'lng_freshgram_')
plural_parts = ('zero', 'one', 'two', 'few', 'many', 'other')
needed = {
    'ru': ('one', 'few', 'many', 'other'),
    'fr': ('one', 'other'),
    'de': ('one', 'other'),
    'it': ('one', 'other'),
    'zh': ('other',),
}


def read(path):
    result = {}
    with open(path, encoding='utf-8') as f:
        for number, line in enumerate(f, 1):
            line = line.rstrip('\n')
            if not line.strip():
                continue
            m = line_re.match(line)
            if not m:
                print('%s:%d: bad line' % (os.path.basename(path), number))
                sys.exit(1)
            if m.group(1) in result:
                print('%s:%d: repeated key %s' % (
                    os.path.basename(path), number, m.group(1)))
                sys.exit(1)
            result[m.group(1)] = m.group(2)
    return result


source = {}
with open(os.path.join(here, '..', 'lang.strings'), encoding='utf-8') as f:
    for line in f:
        m = line_re.match(line.rstrip('\n'))
        if m and m.group(1).startswith(prefixes):
            source[m.group(1)] = m.group(2)
bases = {k.split('#')[0] for k in source}
plural_bases = {k.split('#')[0] for k in source if '#' in k}

failed = False


def fail(lang, key, text):
    global failed
    failed = True
    print('%s: %s: %s' % (lang, key, text))


for lang in sorted(needed):
    path = os.path.join(here, lang + '.strings')
    if not os.path.exists(path):
        fail(lang, '-', 'file is missing')
        continue
    values = read(path)
    for key, value in values.items():
        base, _, part = key.partition('#')
        if base not in bases:
            fail(lang, key, 'unknown key')
            continue
        if part and (base not in plural_bases or part not in plural_parts):
            fail(lang, key, 'unexpected plural form')
            continue
        original = source[key] if key in source else source[base + '#other']
        if set(tag_re.findall(value)) != set(tag_re.findall(original)):
            fail(lang, key, 'placeholders differ')
        if value.count('**') != original.count('**'):
            fail(lang, key, '** markup differs')
        if value.count('\\n') != original.count('\\n'):
            fail(lang, key, 'line breaks differ')
        if re.search(r'(?<!\\)"', value):
            fail(lang, key, 'unescaped quote')
    for base in plural_bases:
        if any(base + '#' + p in values for p in plural_parts):
            for p in needed[lang]:
                if base + '#' + p not in values:
                    fail(lang, base, 'missing plural form ' + p)
    missing = [k for k in source
               if k.split('#')[0] not in {x.split('#')[0] for x in values}]
    print('%s: %d keys, %d not translated' % (lang, len(values), len(missing)))
    if '--missing' in sys.argv:
        for key in missing:
            print('  ' + key)

sys.exit(1 if failed else 0)
