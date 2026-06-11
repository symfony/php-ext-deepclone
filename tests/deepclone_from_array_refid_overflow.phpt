--TEST--
deepclone_from_array() rejects ZEND_LONG_MIN ref ids instead of negating them (signed-overflow UB)
--EXTENSIONS--
deepclone
--FILE--
<?php

function check(string $label, array $payload, bool $allowNamedClosures = false): void {
    try {
        deepclone_from_array($payload, null, $allowNamedClosures);
        echo "$label: NO EXCEPTION\n";
    } catch (\ValueError $e) {
        echo "$label: ValueError: ", $e->getMessage(), "\n";
    } catch (\Throwable $e) {
        echo "$label: ", $e::class, ": ", $e->getMessage(), "\n";
    }
}

// Negating PHP_INT_MIN is not representable in a signed long; each ref-id
// resolution site must reject it before the negation rather than wrap around.

// Top-level "prepared" ref id.
check('prepared min',
    ['classes' => '', 'objectMeta' => 0, 'prepared' => PHP_INT_MIN]);

// dc_resolve() object-reference path (mask marker true, nested under a mask array).
check('object-ref min',
    ['classes' => '', 'objectMeta' => 0,
     'prepared' => [0 => PHP_INT_MIN], 'mask' => [0 => true]]);

// dc_resolve() named-closure path (mask marker int 0, value is [obj_id, method]).
// The reported id is PHP_INT_MIN, whose width is word-size dependent, so the
// --EXPECTF-- line matches it with %i.
check('named-closure min',
    ['classes' => '', 'objectMeta' => 0,
     'prepared' => [PHP_INT_MIN, 'method'], 'mask' => 0],
    allowNamedClosures: true);

// A non-overflowing negative id still produces the ordinary "unknown ref id"
// error, confirming only the ZEND_LONG_MIN wraparound is special-cased.
check('prepared -1',
    ['classes' => '', 'objectMeta' => 0, 'prepared' => -1]);
check('object-ref -1',
    ['classes' => '', 'objectMeta' => 0,
     'prepared' => [0 => -1], 'mask' => [0 => true]]);
check('named-closure -1',
    ['classes' => '', 'objectMeta' => 0,
     'prepared' => [-1, 'method'], 'mask' => 0],
    allowNamedClosures: true);

echo "Done\n";
?>
--EXPECTF--
prepared min: ValueError: deepclone_from_array(): Argument #1 ($data) "prepared" references unknown ref id out of range
object-ref min: ValueError: deepclone_from_array(): malformed payload, ref id out of range
named-closure min: ValueError: deepclone_from_array(): malformed payload, named-closure references unknown id %i
prepared -1: ValueError: deepclone_from_array(): Argument #1 ($data) "prepared" references unknown ref id 1
object-ref -1: ValueError: deepclone_from_array(): malformed payload, unknown ref id 1
named-closure -1: ValueError: deepclone_from_array(): malformed payload, named-closure references unknown id -1
Done
