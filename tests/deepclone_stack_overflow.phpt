--TEST--
deepclone_to_array() throws Error instead of segfaulting on deep nesting
--EXTENSIONS--
deepclone
--SKIPIF--
<?php
// The stack-overflow guard inside dc_copy_value relies on either:
//   (a) ZEND_CHECK_STACK_LIMIT + a non-NULL EG(stack_limit), or
//   (b) the DC_MAX_DEPTH fallback (512 levels).
// When zend.max_allowed_stack_size=0, PHP leaves EG(stack_limit)=NULL
// and path (a) is a no-op. Path (b) still fires at 512 levels, but on
// systems with very deep per-level C-stack usage (e.g. macOS Homebrew
// with --enable-dtrace) a 512-node linked list may overflow the C stack
// before dc_copy_value can run the depth check.
// Skip here when stack protection is explicitly disabled.
if (ini_get('zend.max_allowed_stack_size') == 0) {
    die('skip: zend.max_allowed_stack_size=0 disables PHP stack-limit detection; '
       .'DC_MAX_DEPTH fallback cannot be tested safely on this system');
}
?>
--FILE--
<?php

// Build a linked list deeper than DC_MAX_DEPTH (512).
// Without the fix this caused a C-stack overflow (segfault).
$head = null;
for ($i = 599; $i >= 0; --$i) {
    $node = new stdClass();
    $node->i = $i;
    $node->next = $head;
    $head = $node;
}

try {
    deepclone_to_array($head);
    echo "FAIL: no exception thrown\n";
} catch (Error $e) {
    echo $e->getMessage() . "\n";
}

// A shallow list (depth 10) must still work.
$head2 = null;
for ($i = 9; $i >= 0; --$i) {
    $node = new stdClass();
    $node->i = $i;
    $node->next = $head2;
    $head2 = $node;
}
$arr = deepclone_to_array($head2);
echo "shallow depth ok\n";

// The depth counter must reset between calls: a second deep call must also throw.
try {
    deepclone_to_array($head);
    echo "FAIL: no exception thrown on second call\n";
} catch (Error $e) {
    echo $e->getMessage() . "\n";
}
?>
--EXPECT--
Nesting level too deep - recursive dependency?
shallow depth ok
Nesting level too deep - recursive dependency?
