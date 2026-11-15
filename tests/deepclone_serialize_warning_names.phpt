--TEST--
deepclone_to_array() prints the names of anonymous classes in the warnings it shares with serialize() like PHP prints them
--EXTENSIONS--
deepclone
--FILE--
<?php

function check(object $value): void
{
    $messages = [];
    set_error_handler(static function ($type, $message) use (&$messages) {
        $messages[] = $message;

        return true;
    });
    try {
        deepclone_to_array($value);
    } catch (TypeError $e) {
        $messages[] = $e->getMessage();
    } finally {
        restore_error_handler();
    }
    foreach ($messages as $message) {
        echo strtr($message, ["\0" => '~']), "\n";
        // Since PHP 8.6, names aren't cut at their first NUL byte anymore
        if (str_contains($message, 'class@anonymous') && str_contains($message, "\0") !== PHP_VERSION_ID >= 80600) {
            echo "  the name isn't printed like PHP does\n";
        }
    }
}

// Anonymous classes that restore their state can be exported
check(new class() {
    public function __sleep() { return 'nope'; }
    public function __wakeup(): void {}
});
check(new class() {
    public $a;
    public function __sleep(): array { return [0, 'a']; }
    public function __wakeup(): void {}
});
check(new class() {
    public function __serialize() { return 'nope'; }
    public function __unserialize(array $data): void {}
});

?>
--EXPECTF--
serialize(): class@anonymous%S::__sleep() should return an array only containing the names of instance-variables to serialize
serialize(): class@anonymous%S::__sleep() should return an array only containing the names of instance-variables to serialize
serialize(): "0" returned as member variable from __sleep() but does not exist
class@anonymous%S::__serialize() must return an array
