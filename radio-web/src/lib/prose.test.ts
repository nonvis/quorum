import { describe, expect, it } from 'vitest';
import { blocks, inline } from './prose';

describe('blocks', () => {
  it('splits fences, headings, lists and paragraphs', () => {
    const b = blocks('# Map\n\nmain starts here.\nStill the same paragraph.\n\n```rust\nfn main() {}\n```\n- one\n- two\n1. first');
    expect(b.map((x) => x.t)).toEqual(['h', 'p', 'code', 'ul', 'ol']);
    expect(b[1]).toEqual({ t: 'p', text: 'main starts here. Still the same paragraph.' });
    expect(b[2]).toEqual({ t: 'code', lang: 'rust', text: 'fn main() {}' });
    expect(b[3]).toEqual({ t: 'ul', items: ['one', 'two'] });
  });

  it('keeps an unclosed fence as code', () => {
    expect(blocks('```\nlet x = 1;')).toEqual([{ t: 'code', lang: '', text: 'let x = 1;' }]);
  });
});

describe('inline', () => {
  it('finds code, bold and citations', () => {
    expect(inline('see `listener/src/main.rs:42` and **why** poll.rs:10-12, not v1.2')).toEqual([
      { t: 'text', text: 'see ' },
      { t: 'cite', text: 'listener/src/main.rs:42', path: 'listener/src/main.rs', line: 42, end: null, code: true },
      { t: 'text', text: ' and ' },
      { t: 'bold', text: 'why' },
      { t: 'text', text: ' ' },
      { t: 'cite', text: 'poll.rs:10-12', path: 'poll.rs', line: 10, end: 12, code: false },
      { t: 'text', text: ', not v1.2' },
    ]);
  });

  it('leaves plain inline code alone', () => {
    expect(inline('call `run()` now')).toEqual([
      { t: 'text', text: 'call ' },
      { t: 'code', text: 'run()' },
      { t: 'text', text: ' now' },
    ]);
  });
});
