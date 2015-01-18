# Copyright (c) 2014, 2015 Bert Burgemeister <trebbu@googlemail.com>

# Permission is hereby granted, free of charge, to any person
# obtaining a copy of this software and associated documentation files
# (the "Software"), to deal in the Software without restriction,
# including without limitation the rights to use, copy, modify, merge,
# publish, distribute, sublicense, and/or sell copies of the Software,
# and to permit persons to whom the Software is furnished to do so,
# subject to the following conditions:

# The above copyright notice and this permission notice shall be
# included in all copies or substantial portions of the Software.

# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
# EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
# MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
# NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
# BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
# ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
# CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.

PREFIX = /usr/local
CCFLAGS += -Wall -Wextra -pedantic
CCFLAGS += -std=c99 -D_POSIX_C_SOURCE=200809L
CCFLAGS += `pkg-config --cflags --libs gtk+-3.0 gmodule-2.0`
CC != which cc

pipeglade: pipeglade.c
	$(CC) $< -o $@ $(CCFLAGS)

install: pipeglade pipeglade.1
	mkdir -p $(PREFIX)/bin/
	mkdir -p $(PREFIX)/man/man1/
	cp -f pipeglade $(PREFIX)/bin/
	chmod 755 $(PREFIX)/bin/pipeglade
	gzip -c pipeglade.1 > $(PREFIX)/man/man1/pipeglade.1.gz
	chmod 644 $(PREFIX)/man/man1/pipeglade.1.gz

uninstall:
	rm -f $(PREFIX)/bin/pipeglade
	rm -f $(PREFIX)/man/man1/pipeglade.1.gz

clean:
	rm -f pipeglade
	rm -rf gh-pages

.PHONY: install uninstall clean



# Build targets end here.  The rest of this Makefile is only useful
# for project maintenance.
#
# It works with FreeBSD's version of make (aka pmake).  It won't work
# with GNU make.
######################################################################
VERSION != git describe --tags | cut -d "-" -f 1
CODE_VERSION != awk '/\#define VERSION/{print $$3}' pipeglade.c | tr -d '"'
NEWS_VERSION != awk '/^[0-9]+.[0-9]+.[0-9]+ .*(.+)/{print $$1}' NEWS | head -n1
NEWS_DATE != awk '/^[0-9]+.[0-9]+.[0-9]+ .*(.+)/{print substr($$2, 2, 10)}' NEWS | head -n1
TODAY != date +%F
MANPAGE_DATE != grep "^\.Dd " pipeglade.1
MANPAGE_TODAY != date '+.Dd %B %e, %Y' | awk '{print $$1, $$2, $$3, $$4}'

# Prepare the www directory
gh-pages: gh-pages/index.html gh-pages/pipeglade.1.html

gh-pages/index.html gh-pages/pipeglade.1.html: pipeglade.1 html-template/index.html Makefile
	mkdir -p gh-pages
	cp html-template/* gh-pages/
	mandoc -T html -O style=style.css pipeglade.1 > gh-pages/pipeglade.1.html
	mandoc -T pdf -O paper=a4 pipeglade.1 > gh-pages/pipeglade.1.pdf
	cp LICENSE gh-pages/
	echo -e '/@/\ns/</\&lt;/\ns/>/\&gt;/\n,s/^$$/<p>/\nwq' | ed -s gh-pages/LICENSE
	echo -e '/<!-- replace_with_license_text -->/d\n-r gh-pages/LICENSE\nwq' | ed -s gh-pages/index.html
	echo -e ',s/_PUT_VERSION_HERE_/$(VERSION)/g\nwq' | ed -s gh-pages/index.html
	echo -e '/<\/body>/-r gh-pages/statcounter.html\nwq' | ed -s gh-pages/index.html
	echo -e '/<\/body>/-r gh-pages/statcounter.html\nwq' | ed -s gh-pages/pipeglade.1.html
	echo -e '/<\/body>/-r gh-pages/statcounter.html\nwq' | ed -s gh-pages/404.html
	rm -f gh-pages/statcounter.html gh-pages/LICENSE

# Create a new git tag only if there is a NEWS headline in the format
# 1.2.3 (2015-03-22)
# where 1.2.3 matches the current pipeglade version and the date is of
# today, and if pipeglade.1 has today's date in its .Dd line.
# (NEWS headlines are lines that start at column 0.)
git-tag:
	if test "$(NEWS_DATE)" != "$(TODAY)"; then \
		echo "NEWS: $(NEWS_DATE) != $(TODAY)"; false; \
	fi; \
	if test "$(NEWS_VERSION)" != "$(CODE_VERSION)"; then \
		echo "NEWS: $(NEWS_VERSION) != $(CODE_VERSION)"; false; \
	fi; \
	if test "$(MANPAGE_DATE)" != "$(MANPAGE_TODAY)"; then \
		echo "MANPAGE: $(MANPAGE_DATE) != $(MANPAGE_TODAY)"; false; \
	fi; \
	git tag $(CODE_VERSION);

# Push the www directory to Github Pages
publish: clean gh-pages
	(cd gh-pages; \
	git init; \
	git add ./; \
	git commit -a -m "gh-pages pseudo commit"; \
	git push git@github.com:trebb/pipeglade.git +master:gh-pages)
