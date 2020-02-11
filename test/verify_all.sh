#!/bin/sh

diff_prog="../obj/diff"

diff_type=unidiff

verify_diff_script() {
	orig_left="$1"
	orig_right="$2"
	the_diff="$3"

	verify_left="verify.$orig_left"
	verify_right="verify.$orig_right"

        if [ "x$diff_type" = "xunidiff" ]; then
                cp "$orig_left" "$verify_right"
                patch --quiet -u "$verify_right" "$the_diff"
                if ! cmp "$orig_right" "$verify_right" ; then
                        echo "FAIL: $orig_right != $verify_right"
                        return 1
                fi

                cp "$orig_right" "$verify_left"
                patch --quiet -u -R "$verify_left" "$the_diff"
                if ! cmp "$orig_left" "$verify_left" ; then
                        echo "FAIL: $orig_left != $verify_left"
                        return 1
                fi
        else
                tail -n +3 "$the_diff" | grep -v "^+" | sed 's/^.//' > "$verify_left"
                tail -n +3 "$the_diff" | grep -v "^-" | sed 's/^.//' > "$verify_right"

                if ! cmp "$orig_left" "$verify_left" ; then
                        echo "FAIL: $orig_left != $verify_left"
                        return 1
                fi
                if ! cmp "$orig_right" "$verify_right" ; then
                        echo "FAIL: $orig_right != $verify_right"
                        return 1
                fi
        fi
        echo "OK: $diff_prog $orig_left $orig_right"
        return 0
}

for left in test*.left.* ; do
        right="$(echo "$left" | sed 's/\.left\./.right./')"
        expected_diff="$(echo "$left" | sed 's/test\([0-9]*\)\..*/expect\1.diff/')"
        got_diff="verify.$expected_diff"

        "$diff_prog" "$left" "$right" > "$got_diff"

        set -e
	verify_diff_script "$left" "$right" "$got_diff"
        set +e
done
