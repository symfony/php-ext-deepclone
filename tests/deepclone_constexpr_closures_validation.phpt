--TEST--
deepclone_from_array() validates const-expr-closure payloads
--EXTENSIONS--
deepclone
--SKIPIF--
<?php if (PHP_VERSION_ID < 80500) die('skip requires PHP 8.5'); ?>
--FILE--
<?php

#[Attribute(Attribute::TARGET_ALL)]
class CA { public function __construct(public mixed $cb = null) {} }

#[CA(static function (): string { return 'ok'; })]
class Fix {
    public const TAGGED = 1;
    public string $tagged = 'v';
    public function tagged(int $x = 0): void {}
}

// The reference is [class, site, key, hash]. A hash of 0 means "unverified"; the
// cases below all pass 0, so on PHP 8.6 the engine's own resolution throws and
// the ext heals positionally through its value-walk, surfacing these messages on
// every version.
$cases = [
    'foo',
    [Fix::class],
    [Fix::class, '', 0],
    [Fix::class, '', 0, 0, 0],
    [42, '', 0, 0],
    [Fix::class, 0, 0, 0],
    [Fix::class, '', [], 0],
    [Fix::class, '', 0, '0'],
    ['No\\Such\\ClassAtAll', '', 0, 0],
    [Fix::class, '$nope', 0, 0],
    [Fix::class, 'nope()', 0, 0],
    [Fix::class, 'NOPE', 0, 0],
    [Fix::class, '$tagged::bad()', 0, 0],
    [Fix::class, '$tagged::get()', 0, 0],
    [Fix::class, '', 'x', 0],
    [Fix::class, '', 9, 0],
    [Fix::class, 'tagged()', 9, 0],
];

foreach ($cases as $prepared) {
    try {
        deepclone_from_array(['classes' => '', 'objectMeta' => 0, 'prepared' => $prepared, 'mask' => 1]);
        echo "no error\n";
    } catch (\ValueError $e) {
        echo $e->getMessage(), "\n";
    }
}
?>
--EXPECT--
deepclone_from_array(): malformed payload, const-expr-closure value must be of type array, string given
deepclone_from_array(): malformed payload, const-expr-closure value must have 4 elements
deepclone_from_array(): malformed payload, const-expr-closure value must have 4 elements
deepclone_from_array(): malformed payload, const-expr-closure value must have 4 elements
deepclone_from_array(): malformed payload, const-expr-closure reference must be [string class, string site, int|string key, int hash]
deepclone_from_array(): malformed payload, const-expr-closure reference must be [string class, string site, int|string key, int hash]
deepclone_from_array(): malformed payload, const-expr-closure reference must be [string class, string site, int|string key, int hash]
deepclone_from_array(): malformed payload, const-expr-closure reference must be [string class, string site, int|string key, int hash]
deepclone_from_array(): malformed payload, const-expr-closure references unknown class "No\Such\ClassAtAll"
deepclone_from_array(): malformed payload, const-expr-closure references unknown property "$nope"
deepclone_from_array(): malformed payload, const-expr-closure references unknown method "nope()"
deepclone_from_array(): malformed payload, const-expr-closure references unknown constant "NOPE"
deepclone_from_array(): malformed payload, const-expr-closure references unknown hook "$tagged::bad()"
deepclone_from_array(): malformed payload, const-expr-closure references unknown hook "$tagged::get()"
deepclone_from_array(): malformed payload, const-expr-closure references unknown closure at site "" of class "Fix"
deepclone_from_array(): malformed payload, const-expr-closure references unknown closure "@9" in class "Fix"
deepclone_from_array(): malformed payload, const-expr-closure references unknown closure "tagged()@9" in class "Fix"
