# Credits

## This project

All gameplay behaviour in this repository - the host-authoritative entity sync, the player puppet, the
combat/kill path (engine-native death via `CSrvNPCHealth::SetLife`, the combat is-alive service flag,
corpse handling), the guest->host kill/damage relay, identity via `HandleContainer::m_ObjectID`, the
swim/locomotion driving, the decompilation audit and the planning documents - is original work by
Davidoff-byte.

## Initial framework: replaced

The repository started from the initial commit of Kamzik123/AC4Coop (project skeleton, `Hook`/`HookMath`
helpers, `Scimitar` class declarations). That skeleton has now been **fully replaced**: every file under
`src/`, plus the project files, has been independently rewritten, with the layouts, virtual slots and
entry points re-derived from our own decompilation of `AC4BFSP.exe`. `git diff <initial commit> -- src/`
leaves no untouched file. The per-file record is in `docs/MIGRATION-OFF-UPSTREAM.md`.

The upstream commit remains in `git log` as history and nothing else; no upstream text is present in the
working tree.

No license file was present in the upstream repository. Because no upstream code remains in the tree, the
current sources are not redistributed under their terms; the upstream history is retained only for
provenance. The engine offsets and behaviour notes are facts about the game binary, established by
reverse engineering for interoperability.
