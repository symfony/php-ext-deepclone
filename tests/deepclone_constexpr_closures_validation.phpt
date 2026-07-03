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

$line = (new ReflectionFunction((new ReflectionClass(Fix::class))->getAttributes()[0]->getArguments()[0]))->getStartLine();

$cases = [
    'foo',
    [Fix::class],
    [Fix::class, '@0', $line, 'x'],
    [42, '@0', $line],
    [Fix::class, 0, $line],
    [Fix::class, '@0', 'x'],
    [Fix::class, 'nope', $line],
    [Fix::class, '@', $line],
    [Fix::class, '@01', $line],
    [Fix::class, '@1x', $line],
    ['No\\Such\\ClassAtAll', '@0', $line],
    [Fix::class, '$nope@0', $line],
    [Fix::class, 'nope()@0', $line],
    [Fix::class, 'NOPE@0', $line],
    [Fix::class, '$tagged::bad()@0', $line],
    [Fix::class, '$tagged::get()@0', $line],
    [Fix::class, '@9', $line],
    [Fix::class, 'tagged()@9', $line],
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
deepclone_from_array(): malformed payload, const-expr-closure value must have 3 elements
deepclone_from_array(): malformed payload, const-expr-closure value must have 3 elements
deepclone_from_array(): malformed payload, const-expr-closure class name must be of type string, int given
deepclone_from_array(): malformed payload, const-expr-closure id must be of type string, int given
deepclone_from_array(): malformed payload, const-expr-closure line must be of type int, string given
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
