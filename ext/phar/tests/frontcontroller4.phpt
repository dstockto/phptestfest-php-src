--TEST--
Phar front controller phps
--SKIPIF--
<?php if (!extension_loaded("phar")) die("skip"); ?>
--ENV--
SCRIPT_NAME=/frontcontroller4.php
REQUEST_URI=/frontcontroller4.php
--FILE_EXTERNAL--
frontcontroller.phar
--EXPECTHEADERS--
Status: 301 Moved Permanently
Location: /frontcontroller4.php/index.php
--EXPECT--
