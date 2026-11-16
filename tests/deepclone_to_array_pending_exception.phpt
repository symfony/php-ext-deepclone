--TEST--
deepclone_to_array() stops at the exception that an error handler throws while it reads the state of an object
--EXTENSIONS--
deepclone
--FILE--
<?php

set_error_handler(function (int $type, string $message) { throw new ErrorException($message); });

class State
{
    public function __serialize(): array { return ["\0bad" => 1, 'ref' => WeakReference::create($this)]; }
}

try {
    deepclone_to_array(new State());
    echo "no exception\n";
} catch (Throwable $e) {
    echo get_class($e), ': ', $e->getMessage(), "\n";
    var_dump($e->getPrevious());
}

?>
--EXPECT--
ErrorException: Corrupt member variable name
NULL
