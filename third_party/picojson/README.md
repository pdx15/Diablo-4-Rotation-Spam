# picojson

`picojson.h` from **v1.3.0**, commit
`25fc213cca61ea22b3c2e4db8def9927562ba5f7`:
https://github.com/kazuho/picojson/tree/v1.3.0

Only trailing whitespace has been normalized; library logic is unchanged.

BSD-2-Clause license is retained in the header and in
`THIRD_PARTY_NOTICES.txt`, which is included in release archives.

Used only by `event_schedule.cpp` to parse the Helltides.com response and disk
cache. Input size, nesting depth, timestamps and required arrays are validated
before accepting a schedule. No network dependencies are needed at build time.
