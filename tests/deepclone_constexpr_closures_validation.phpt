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
    [42, '', 0, 0, $line],
    ['No\Such\ClassAtAll', '', 0, 0, $line],
    [Fix::class, 42, 0, 0, $line],
    [Fix::class, '', 'x', 0, $line],
    [Fix::class, '', 0, 'x', $line],
    [Fix::class, '', 0, 0, 'x'],
    [Fix::class, '$nope', 0, 0, $line],
    [Fix::class, 'nope()', 0, 0, $line],
    [Fix::class, 'NOPE', 0, 0, $line],
    [Fix::class, 'tagged()#9', 0, 0, $line],
    [Fix::class, 'tagged()#01', 0, 0, $line],
    [Fix::class, '', 9, 0, $line],
    [Fix::class, '', -1, 0, $line],
    [Fix::class, '', 0, 9, $line],
    [Fix::class, '', 0, -1, $line],
    [Fix::class, '', null, 0, $line],
    [Fix::class, 'tagged()', null, 0, $line],
    [Fix::class, '$tagged::get()', 0, 0, $line],
    [Fix::class, '$tagged::bad()', 0, 0, $line],
    // an int element 1 makes the payload an engine-id reference [class, id, line]
    [Fix::class, 0],
    [Fix::class, 0, $line, 'x'],
    [42, 0, $line],
    [Fix::class, 0, 'x'],
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
deepclone_from_array(): malformed payload, const-expr-closure value must have 5 elements
deepclone_from_array(): malformed payload, const-expr-closure class name must be of type string, int given
deepclone_from_array(): malformed payload, const-expr-closure references unknown class "No\Such\ClassAtAll"
deepclone_from_array(): malformed payload, const-expr-closure value must have 3 elements
deepclone_from_array(): malformed payload, const-expr-closure attribute index must be of type int or null, string given
deepclone_from_array(): malformed payload, const-expr-closure closure index must be of type int, string given
deepclone_from_array(): malformed payload, const-expr-closure line must be of type int, string given
deepclone_from_array(): malformed payload, const-expr-closure references unknown property "$nope"
deepclone_from_array(): malformed payload, const-expr-closure references unknown method "nope()"
deepclone_from_array(): malformed payload, const-expr-closure references unknown constant "NOPE"
deepclone_from_array(): malformed payload, const-expr-closure references unknown parameter "tagged()#9"
deepclone_from_array(): malformed payload, const-expr-closure references unknown parameter "tagged()#01"
deepclone_from_array(): malformed payload, const-expr-closure references unknown attribute index 9
deepclone_from_array(): malformed payload, const-expr-closure references unknown attribute index -1
deepclone_from_array(): malformed payload, const-expr-closure references unknown closure index 9
deepclone_from_array(): malformed payload, const-expr-closure references unknown closure index -1
deepclone_from_array(): malformed payload, const-expr-closure attribute index is required for site ""
deepclone_from_array(): malformed payload, const-expr-closure attribute index is required for site "tagged()"
deepclone_from_array(): malformed payload, const-expr-closure references unknown hook "$tagged::get()"
deepclone_from_array(): malformed payload, const-expr-closure references unknown hook "$tagged::bad()"
deepclone_from_array(): malformed payload, const-expr-closure value must have 3 elements
deepclone_from_array(): malformed payload, const-expr-closure value must have 3 elements
deepclone_from_array(): malformed payload, const-expr-closure class name must be of type string, int given
deepclone_from_array(): malformed payload, const-expr-closure line must be of type int, string given
