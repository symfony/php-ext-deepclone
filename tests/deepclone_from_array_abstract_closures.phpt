--TEST--
deepclone_from_array() doesn't create closures over abstract methods
--EXTENSIONS--
deepclone
--FILE--
<?php

interface Shape
{
    public function area();
    public static function create();
}

abstract class Base implements Shape
{
    abstract protected function name();
    public function area() { return 1; }
    public static function create() { return new static(); }
}

class Square extends Base implements Countable
{
    protected function name() { return 'square'; }
    public function count(): int { return 4; }
}

function check(array $callable, $classes = 'Square', $meta = 1): void
{
    try {
        $closure = deepclone_from_array(['classes' => $classes, 'objectMeta' => $meta, 'prepared' => $callable, 'mask' => 0], null, true);
        var_dump($closure());
    } catch (ValueError $e) {
        echo $e->getMessage(), "\n";
    }
}

// The methods that the class of the object implements
check([0, 'count']);
check([[0, 'name'], 'Square', 'name']);

// The abstract methods they implement can't be called
check([[0, 'count'], 'Countable', 'count']);
check([[0, 'area'], 'Shape', 'area']);
check([[0, 'name'], 'Base', 'name']);
check(['Shape', 'create'], '', 0);
check([['Square', 'create'], 'Shape', 'create'], '', 0);

?>
--EXPECT--
int(4)
string(6) "square"
deepclone_from_array(): malformed payload, named-closure method Countable::count() is abstract
deepclone_from_array(): malformed payload, named-closure method Shape::area() is abstract
deepclone_from_array(): malformed payload, named-closure method Base::name() is abstract
deepclone_from_array(): malformed payload, named-closure method Shape::create() is abstract
deepclone_from_array(): malformed payload, named-closure method Shape::create() is abstract
