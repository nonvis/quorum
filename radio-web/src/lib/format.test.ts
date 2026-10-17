import { describe, expect, it } from 'vitest';
import { activeLine, bucket, clock, retention, when, wordCount } from './format';

const NOW = new Date('2026-10-17T05:00:00Z'); // 14:00 KST

describe('clock', () => {
  it('formats seconds for the player', () => {
    expect(clock(7)).toBe('0:07');
    expect(clock(154.6)).toBe('2:34');
    expect(clock(3723)).toBe('1:02:03');
    expect(clock(NaN)).toBe('0:00');
  });
});

describe('when', () => {
  it('reads in KST', () => {
    expect(when('2026-10-17T04:05:00Z', NOW)).toBe('Today 13:05');
    expect(when('2026-10-16T13:10:00Z', NOW)).toBe('Yesterday 22:10');
    expect(when('2026-10-16T14:59:00Z', NOW)).toBe('Yesterday 23:59');
    expect(when('2026-10-16T15:01:00Z', NOW)).toBe('Today 00:01');
    expect(when('2026-10-14T00:12:00Z', NOW)).toMatch(/^Wed 14 Oct, 09:12$/);
  });
});

describe('retention', () => {
  it('says what happens if nothing is done', () => {
    expect(retention({ kept: true, days_left: null })).toBe('kept');
    expect(retention({ kept: false, days_left: 7 })).toBe('7 days left');
    expect(retention({ kept: false, days_left: 1 })).toBe('1 day left');
    expect(retention({ kept: false, days_left: 0 })).toBe('expires today');
  });
});

describe('activeLine', () => {
  const audio = {
    duration: 30,
    lines: [
      { i: 0, speaker: 'host', start: 0.7, end: 5 },
      { i: 1, speaker: 'host', start: 6, end: 8 },
      { i: 3, speaker: 'builder', start: 9.5, end: 20 },
    ],
  };
  it('follows the audio', () => {
    expect(activeLine(audio, 0)).toBeNull();
    expect(activeLine(audio, 1)).toBe(0);
    expect(activeLine(audio, 5.5)).toBe(0);
    expect(activeLine(audio, 9.5)).toBe(3);
    expect(activeLine(audio, 99)).toBe(3);
    expect(activeLine(null, 3)).toBeNull();
  });
});

describe('bucket', () => {
  it('groups by KST day', () => {
    expect(bucket('2026-10-17T00:00:00Z', NOW)).toBe('Today');
    expect(bucket('2026-10-16T14:00:00Z', NOW)).toBe('Yesterday');
    expect(bucket('2026-10-13T05:00:00Z', NOW)).toBe('This week');
    expect(bucket('2026-10-01T05:00:00Z', NOW)).toBe('Earlier');
  });
});

describe('wordCount', () => {
  it('counts words', () => {
    expect(wordCount('  ')).toBe(0);
    expect(wordCount('Should the gate run first?')).toBe(5);
  });
});
