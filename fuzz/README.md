# `deepclone_from_array` libFuzzer target

The harness converts fuzzer bytes into structured PHP values and calls
`deepclone_from_array()` through PHP's fuzzer SAPI.

## Input shape

Every non-empty input becomes one call with this overall shape. Question marks
mark optional fields.

```php
deepclone_from_array([
    'classes'    => string|array,
    'objectMeta' => int|array,
    'prepared'   => mixed,
    'mask'       => mixed,  // optional parallel marker for prepared
    'properties' => [scope => [property => [objectId => mixed]]], // optional
    'resolve'    => [scope => [property => [objectId => marker]]], // optional
    'refs'       => [referenceId => mixed],                         // optional
    'refMasks'   => [referenceId => marker],                        // optional
    'states'     => [objectId|[objectId, state, marker?], ...],     // optional
], $allowedClasses, $allowNamedClosures);
```

## Build

Build and install a non-ZTS `php-src` with Clang, ASan, UBSan, and
`--enable-fuzzer`. Build this extension with that installation's `phpize` and
`php-config`, using the same sanitizer flags and `-fsanitize=fuzzer-no-link`.

Then build the fuzzer:

```sh
PHP_SRC=/path/to/php-src ./fuzz/build.sh
```

`modules/deepclone.so` must exist before running `build.sh`.

## Run

The corpus and artifact directories are pre-created, while their generated
contents are ignored by Git.

```sh
./fuzz/run.sh \
    -max_len=4096 -timeout=5 -rss_limit_mb=4096 \
    -artifact_prefix=fuzz/artifacts/ fuzz/corpus/from_array
```

Set `DEEPCLONE_FUZZ_EXTENSION` to load a different `deepclone.so`. To inspect
an artifact's generated PHP payload without calling the extension, run:

```sh
DEEPCLONE_FUZZ_DUMP=1 ./fuzz/run.sh -runs=1 ARTIFACT
```

## Ignore list

To keep libFuzzer focused on the extension, add this to `php-src`'s `CFLAGS`
and `CXXFLAGS` before configuring it:

```sh
-fsanitize-coverage-ignorelist=/path/to/php-ext-deepclone/fuzz/native-coverage.ignorelist
```

The ignore list removes native `php_var_unserialize()` edges and comparisons
from coverage feedback. Native unserialization remains ASan/UBSan-instrumented.
