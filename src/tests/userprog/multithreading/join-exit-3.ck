# -*- perl -*-
use strict;
use warnings;
use tests::tests;
check_expected (IGNORE_USER_FAULTS => 1, [<<'EOF']);
(join-exit-3) begin
(join-exit-3) Main starting
(join-exit-3) Thread starting
join-exit-3: exit(162)
EOF
pass;
