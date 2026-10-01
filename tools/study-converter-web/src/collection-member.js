// APKG v2/v3 may also carry a valid but near-empty collection.anki2 as a
// compatibility placeholder. Select by package version, never by ZIP order.
export function selectCollectionMember(archive, metadataVersion = null) {
  if (!archive['collection.anki2'] && !archive['collection.anki21'] && !archive['collection.anki21b']) {
    throw new Error('Este arquivo não é um pacote Anki (.apkg) — não tem coleção de cartões. Se ele veio deste conversor (.zip), já está convertido: extraia e copie o .cps para /study/.');
  }
  const version = metadataVersion ?? (archive['collection.anki21b'] ? 3 : archive['collection.anki21'] ? 2 : 1);
  if (version < 3 && archive['collection.anki21b']) throw new Error('Metadados conflitantes com a coleção moderna.');
  const key = version >= 3 ? 'collection.anki21b' : version === 2 ? 'collection.anki21' : 'collection.anki2';
  if (!archive[key]) throw new Error(`Coleção ${key} não encontrada no pacote.`);
  return { key, version };
}
