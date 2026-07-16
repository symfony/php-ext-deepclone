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

$cases = [
    'foo',
    [Fix::class],
    [Fix::class, '@0', 1],
    [42, '@0'],
    [Fix::class, 0],
    [Fix::class, 'nope'],
    [Fix::class, '@'],
    [Fix::class, '@01'],
    [Fix::class, '@1x'],
    ['No\\Such\\ClassAtAll', '@0'],
    [Fix::class, '$nope@0'],
    [Fix::class, 'nope()@0'],
    [Fix::class, 'NOPE@0'],
    [Fix::class, '$tagged::bad()@0'],
    [Fix::class, '$tagged::get()@0'],
    [Fix::class, '@9'],
    [Fix::class, 'tagged()@9'],
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
deepclone_from_array(): malformed payload, const-expr-closure value must have 2 elements
deepclone_from_array(): malformed payload, const-expr-closure value must have 2 elements
deepclone_from_array(): malformed payload, const-expr-closure class name must be of type string, int given
deepclone_from_array(): malformed payload, const-expr-closure id must be of type string, int given
deepclone_from_array(): malformed payload, const-expr-closure id must be of the form "<site>@<rank>", "nope" given
deepclone_from_array(): malformed payload, const-expr-closure id must be of the form "<site>@<rank>", "@" given
deepclone_from_array(): malformed payload, const-expr-closure id must be of the form "<site>@<rank>", "@01" given
deepclone_from_array(): malformed payload, const-expr-closure id must be of the form "<site>@<rank>", "@1x" given
deepclone_from_array(): malformed payload, const-expr-closure references unknown class "No\Such\ClassAtAll"
deepclone_from_array(): malformed payload, const-expr-closure references unknown property "$nope"
deepclone_from_array(): malformed payload, const-expr-closure references unknown method "nope()"
deepclone_from_array(): malformed payload, const-expr-closure references unknown constant "NOPE"
deepclone_from_array(): malformed payload, const-expr-closure references unknown hook "$tagged::bad()"
deepclone_from_array(): malformed payload, const-expr-closure references unknown hook "$tagged::get()"
deepclone_from_array(): malformed payload, const-expr-closure references unknown closure id "@9" in class "Fix"
deepclone_from_array(): malformed payload, const-expr-closure references unknown closure id "tagged()@9" in class "Fix"
