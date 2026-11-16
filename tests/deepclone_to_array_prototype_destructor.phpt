--TEST--
deepclone_to_array() doesn't run the destructor of the instances it creates to read default values
--EXTENSIONS--
deepclone
--FILE--
<?php

class Connection
{
    private string $dsn;
    public $tag;

    public function __construct(string $dsn) { $this->dsn = $dsn; }
    public function __destruct() { echo "closing {$this->dsn}\n"; }
}

// The properties of an object that has dynamic properties, or that was cast to
// array, are compared to those of a new instance
$a = new Connection('a');
foreach ($a as $k => $v) {}
$payload = deepclone_to_array($a);
echo "exported\n";

#[AllowDynamicProperties]
class Dynamic extends Connection {}

$b = new Dynamic('b');
$b->dyn = 1;
$payload = deepclone_to_array($b);
echo "exported\n";

?>
--EXPECT--
exported
exported
closing b
closing a
