--TEST--
deepclone_to_array() exports the properties of many classes with their scope and name
--EXTENSIONS--
deepclone
--FILE--
<?php

class Base
{
    private $secret = 'base';
    protected $kind = 'base';
    protected $widened = 0;
    public $id = 0;

    public function secrets(): array
    {
        return [$this->secret];
    }
}

// More classes than the walk remembers at once, with properties of each visibility
for ($i = 0; $i < 40; ++$i) {
    $wakeup = $i % 3 ? '' : 'public static $woken = 0; public function __wakeup(): void { ++self::$woken; }';
    $sleep = $i % 5 ? '' : 'public function __sleep(): array { return ["id", "kind", "own'.$i.'", "\0C'.$i.'\0secret", "\0Base\0secret"]; }';
    eval("class C$i extends Base
    {
        private \$secret = 'c$i';
        protected \$own$i = null;
        public \$widened = $i;
        $wakeup
        $sleep

        public function __construct()
        {
            \$this->id = $i;
            \$this->kind = 'kind$i';
            \$this->own$i = [$i];
        }

        public function secrets(): array
        {
            return [\$this->secret, ...parent::secrets()];
        }
    }");
}

$graph = [];
for ($round = 0; $round < 3; ++$round) {
    for ($i = 0; $i < 40; ++$i) {
        $graph[] = new ("C$i")();
    }
}

$payload = deepclone_to_array($graph);
$clone = deepclone_from_array($payload);

var_dump($clone == $graph);
var_dump(count($payload['properties']));
var_dump(array_keys($payload['properties']['C7']), array_keys($payload['properties']['Base']));
var_dump($clone[47]->secrets(), C0::$woken, C3::$woken, isset(C1::$woken));

?>
--EXPECT--
bool(true)
int(42)
array(1) {
  [0]=>
  string(4) "own7"
}
array(1) {
  [0]=>
  string(4) "kind"
}
array(2) {
  [0]=>
  string(2) "c7"
  [1]=>
  string(4) "base"
}
int(3)
int(3)
bool(false)
