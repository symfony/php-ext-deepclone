--TEST--
deepclone_to_array() orders scopes like the polyfill when an uninitialized typed property precedes them
--EXTENSIONS--
deepclone
--FILE--
<?php

class C
{
    public int $id;
    private $p = 0;
    public $q = 0;

    public function __construct()
    {
        $this->p = 1;
        $this->q = 1;
    }
}

$c = new C();
echo json_encode(deepclone_to_array($c)['properties']), "\n";

// Same once the property table is built
foreach ($c as $v) {
}
echo json_encode(deepclone_to_array($c)['properties']), "\n";
?>
--EXPECT--
{"C":{"p":[1]},"stdClass":{"q":[1]}}
{"C":{"p":[1]},"stdClass":{"q":[1]}}
