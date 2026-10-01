import { test } from 'node:test';
import { strict as assert } from 'node:assert';
import { selectCollectionMember } from '../src/collection-member.js';

const sqlite = new Uint8Array([83, 81, 76]);

test('Anki v2 selects collection.anki21 instead of the valid SQLite decoy', () => {
  const archive = { 'collection.anki2': sqlite, 'collection.anki21': sqlite };
  assert.equal(selectCollectionMember(archive, 2).key, 'collection.anki21');
  assert.equal(selectCollectionMember(archive).key, 'collection.anki21');
});

test('Anki v3 prefers the compressed collection over other SQLite members', () => {
  const archive = { 'collection.anki2': sqlite, 'collection.anki21': sqlite, 'collection.anki21b': sqlite };
  assert.equal(selectCollectionMember(archive, 3).key, 'collection.anki21b');
  assert.equal(selectCollectionMember(archive).key, 'collection.anki21b');
  assert.throws(() => selectCollectionMember(archive, 2), /Metadados conflitantes/);
});

test('legacy packages select the legacy collection and incompatible v2 fails instead of using a decoy', () => {
  const archive = { 'collection.anki2': sqlite };
  assert.equal(selectCollectionMember(archive, 1).key, 'collection.anki2');
  assert.throws(() => selectCollectionMember(archive, 2), /collection.anki21 não encontrada/);
});

test('archives without any collection get a friendly not-an-anki-package error', () => {
  assert.throws(() => selectCollectionMember({ 'media': sqlite }), /não é um pacote Anki/);
});
