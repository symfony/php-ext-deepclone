--TEST--
deepclone_to_array() leaves no placeholder or empty entry in masks
--EXTENSIONS--
deepclone
--FILE--
<?php

class MaskHolder
{
    public $a;
}

class MaskState
{
    public $data;

    public function __serialize(): array
    {
        return $this->data;
    }

    public function __unserialize(array $data): void
    {
        $this->data = $data;
    }
}

// A shared reference to a scalar needs no ref mask
$v = [1];
$v[] = &$v[0];
var_dump(isset(deepclone_to_array($v)['refMasks']));

// Unwrapping a reference seen once leaves no placeholder behind, in property
// masks, state masks and the masks of shared reference values
$x = 1;
$o = new MaskHolder();
$o->a = [&$x, new stdClass()];
echo json_encode(deepclone_to_array($o)['resolve']), "\n";

$y = 1;
$s = new MaskState();
$s->data = ['k' => [&$y, new stdClass()]];
echo json_encode(deepclone_to_array($s)['states']), "\n";

$z = 1;
$w = ['k' => [&$z, new stdClass()]];
echo json_encode(deepclone_to_array([&$w, &$w])['refMasks']), "\n";
?>
--EXPECT--
bool(false)
{"stdClass":{"a":[{"1":true}]}}
{"2":[0,{"k":[1,1]},{"k":{"1":true}}]}
{"1":{"k":{"1":true}}}
