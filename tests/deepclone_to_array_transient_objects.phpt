--TEST--
deepclone_to_array() tells apart the objects that __serialize() or Serializable::serialize() create and release
--EXTENSIONS--
deepclone
--FILE--
<?php

class Fresh
{
    public function __construct(public $n) {}
    public function __serialize(): array { return ['o' => (object) ['n' => $this->n]]; }
    public function __unserialize(array $data): void { $this->n = $data['o']->n; }
}

$c = deepclone_from_array(deepclone_to_array([new Fresh(1), new Fresh(2)]));
var_dump($c[0]->n, $c[1]->n);

// DatePeriod::__serialize() creates new dates on each call
$periods = [
    new DatePeriod(new DateTime('2020-01-01 UTC'), new DateInterval('P1D'), 1),
    new DatePeriod(new DateTime('2021-01-01 UTC'), new DateInterval('P1W'), 1),
];
foreach (deepclone_from_array(deepclone_to_array($periods)) as $p) {
    echo implode(', ', array_map(fn ($d) => $d->format('Y-m-d'), iterator_to_array($p))), "\n";
}

// Like serialize(), an object that Serializable::serialize() meets again is a back-reference
@eval('class OldSer implements Serializable {
    public $x;
    public function serialize(): string { return serialize([$this->x]); }
    public function unserialize($data): void { [$this->x] = unserialize($data); }
}');
$o = new OldSer();
$t = new stdClass();
$o->x = (object) ['self' => $t];
$t->arr = [$o];
unset($o);
$c = deepclone_from_array(deepclone_to_array([$t]));
var_dump($c[0]->arr[0]->x->self->arr[0] === $c[0]->arr[0]);
?>
--EXPECT--
int(1)
int(2)
2020-01-01, 2020-01-02
2021-01-01, 2021-01-08
bool(true)
