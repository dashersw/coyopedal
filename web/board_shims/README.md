# Board shims

The headers here exist so the board's OWN sources compile for the browser:
`src/native/ui/board_bridge.cpp`, `src/native/ui/controls.c`,
`src/native/storage/{model_catalog.c,factory_presets.c,panel_presets.cpp}`.
Those files are the pedal -- the amp values, the preset records, the revision
hash, the dirty flag the save prompt reads -- and the web build runs them
unmodified rather than reimplementing them.

What each header stands in for is an ESP-IDF facility the browser has its own
answer to, and nothing more: a log line, a critical section on a single-threaded
UI, a heap with no SPIRAM, a key-value store, a microsecond clock. The bodies
are in `web/board_platform.cpp`, next to the flash store the two factory images
are served from.

None of these is a model of ESP-IDF. Each is the narrowest thing that lets the
real file through, and anything a browser genuinely cannot answer is a stub that
says so rather than a pretence.
