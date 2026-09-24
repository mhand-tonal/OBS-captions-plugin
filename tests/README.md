# Stability regression tests

Configure a normal plugin build with `-DBUILD_CAPTIONS_TESTS=ON`, build it, then run:

```sh
ctest --test-dir path/to/build --output-on-failure
```

The tests require the same OBS and Qt libraries as the plugin, but no running OBS
instance, microphone, API key, or network access. They exercise transcript queue
draining (TXT, plain TXT, raw, and SRT), preservation of the last interim caption,
latest-caption file shutdown, writer startup failures, UTF-8 caption byte limits,
and sharing the logging sink across translation units.

On Windows, the matching OBS/Qt runtime DLLs must be on `PATH` when running the tests.
These tests do not establish live transcription or scene-switching compatibility;
those still need to be exercised in OBS.
