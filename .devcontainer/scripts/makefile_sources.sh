# Helpers for reading source lists out of Makefile.am; meant to be sourced.

# Print the fully expanded value of a Makefile.am variable.
am_var() {
	awk -v want="$1" '
	function expand(s,   depth, name) {
		while (match(s, /\$\([A-Za-z0-9_]+\)/)) {
			name = substr(s, RSTART + 2, RLENGTH - 3)
			s = substr(s, 1, RSTART - 1) vars[name] substr(s, RSTART + RLENGTH)
			if (++depth > 50) break
		}
		return s
	}
	{
		line = $0
		while (line ~ /\\$/) {
			sub(/\\$/, "", line)
			if ((getline nxt) <= 0) break
			line = line " " nxt
		}
		if (match(line, /^[A-Za-z_][A-Za-z0-9_]*[ \t]*\+?=/)) {
			lhs = substr(line, 1, RLENGTH)
			rhs = substr(line, RLENGTH + 1)
			append = (lhs ~ /\+=$/)
			sub(/[ \t]*\+?=$/, "", lhs)
			vars[lhs] = append ? vars[lhs] " " rhs : rhs
		}
	}
	END { print expand(vars[want]) }
	' "${MAKEFILE_AM:-Makefile.am}"
}

# Print the .c files listed in a Makefile.am variable, one per line.
am_sources() {
	am_var "$1" | tr -s ' \t' '\n\n' | awk '/\.c$/'
}

# Print the Automake-mangled form of a program name (tests/foo-bar -> tests_foo_bar).
am_canon() {
	printf '%s\n' "$1" | tr -c 'A-Za-z0-9@\n' '_'
}
