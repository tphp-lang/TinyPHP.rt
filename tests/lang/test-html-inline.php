<html><body>
before
<?php echo "one"; ?>
middle
<?php echo "two\n"; ?>
after
</body></html>
<?php
// EXPECT: <html><body>
// EXPECT: before
// EXPECT: onemiddle
// EXPECT: two
// EXPECT: after
// EXPECT: </body></html>
