# Copyright (c) 2014-2016 Bert Burgemeister <trebbu@googlemail.com>

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

prefix ?= /usr/local
bindir ?= $(prefix)/bin
mandir ?= $(prefix)/man
man1dir ?= $(mandir)/man1
CCFLAGS += -Wall -Wextra -pedantic -g
# FreeBSD:
# Suppressing warning: named variadic macros are a GNU extension
# in /usr/local/include/X11/Xfuncproto.h:157:24:
CCFLAGS += -Wno-variadic-macros
CCFLAGS += -std=c99
CCFLAGS += -D_POSIX_C_SOURCE=200809L
CCFLAGS += -D_XOPEN_SOURCE=700
CCFLAGS += `pkg-config --cflags --libs gtk+-3.0 gmodule-2.0`
CCFLAGS += `pkg-config --cflags --libs gtk+-unix-print-3.0`
CCFLAGS += `pkg-config --cflags --libs libxml-2.0`
CC != which cc


all: pipeglade

pipeglade: pipeglade.c Makefile
	$(CC) $< -o $@ $(CCFLAGS)

install: pipeglade pipeglade.1
	install -d $(bindir) $(man1dir)
	install pipeglade $(bindir)
	install -m644 pipeglade.1 $(man1dir)

uninstall:
	rm -f $(bindir)/pipeglade
	rm -f $(man1dir)/pipeglade.1.gz

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
VERSION != (which git >/dev/null && git describe --tags || echo "NONE") | cut -d "-" -f 1
CODE_VERSION != awk '/\#define VERSION/{print $$3}' pipeglade.c | tr -d '"'
NEWS_VERSION != awk '/^[0-9]+.[0-9]+.[0-9]+ .*(.+)/{print $$1}' NEWS | head -n1
NEWS_DATE != awk '/^[0-9]+.[0-9]+.[0-9]+ .*(.+)/{print substr($$2, 2, 10)}' NEWS | head -n1
TODAY != date +%F
MANPAGE_DATE != grep "^\.Dd " pipeglade.1
MANPAGE_TODAY != date '+.Dd %B %e, %Y' | awk '{print $$1, $$2, $$3, $$4}'
.SUFFIXES: .ui .svg .jpg


# Extract a list of actions from source code...
prog-actions:
	@awk -F\" '/eql\((ud->)?action, \"[a-zA-Z0-9_-]+\"/{print $$2}' pipeglade.c | sort -u

# ... and from manual page
man-actions:
	@awk -F: '/Cm :[a-zA-Z0-9_-]+/{print $$2}' pipeglade.1 | awk '{print $$1}' | sort -u

# Extract from manual page a list of subsections on widgets...
man-widgets:
	@awk '/\.Ss Gtk[A-Z][a-zA-Z]+$$/{print $$2}' pipeglade.1 | sort -u

# ... and the respective TOC entries
man-toc:
	@awk '/BEGIN_TOC/,/END_TOC/' pipeglade.1 | awk '/\.Sx Gtk[A-Z][a-zA-Z]+ [,.]$$/{print $$2}'

# Our collection of simple test widgets
examples-list:
	@ls -1 widget-examples | grep "\.ui$$"

# Items marked done in list of all widgets
done-list:
	@awk '!/^#/&&/Gtk[A-Z][a-zA-Z]/&&/done/{print $$1}' buildables.txt


# Prepare the www directory
gh-pages: clean gh-pages/index.html gh-pages/pipeglade.1.html gh-pages/clock.svg ${SNAPSHOTS}

gh-pages/index.html: pipeglade gh-pages/pipeglade.1.html www-template/index.html Makefile gh-pages/gallery.html
	cp clock.sh gh-pages/clock.sh.txt
	cp clock.ui gh-pages/clock.ui.txt
	cp LICENSE gh-pages/
	echo -e '/@/\ns/</\&lt;/\ns/>/\&gt;/\n,s/^$$/<p>/\nwq' | ed -s gh-pages/LICENSE
	echo -e '/<!-- replace_with_widget_gallery -->/d\n-r gh-pages/gallery.html\nwq' | ed -s gh-pages/index.html
	echo -e '/<!-- replace_with_license_text -->/d\n-r gh-pages/LICENSE\nwq' | ed -s gh-pages/index.html
	echo -e ',s/_PUT_VERSION_HERE_/$(VERSION)/g\nwq' | ed -s gh-pages/index.html
	echo -e '/<\/body>/-r gh-pages/statcounter.html\nwq' | ed -s gh-pages/index.html
	echo -e '/<\/body>/-r gh-pages/statcounter.html\nwq' | ed -s gh-pages/404.html
	rm -f gh-pages/statcounter.html gh-pages/LICENSE

gh-pages/pipeglade.1.html: pipeglade.1 Makefile
	mkdir -p gh-pages
	cp www-template/* gh-pages/
	mandoc -Wall -T xhtml -O style=style.css pipeglade.1 > gh-pages/pipeglade.1.html
	mandoc -Wall -T pdf -O paper=a4 pipeglade.1 > gh-pages/pipeglade.1.pdf
	echo -e '/<\/body>/-r gh-pages/statcounter.html\nwq' | ed -s gh-pages/pipeglade.1.html

gh-pages/clock.svg: clock.sh clock.ui pipeglade
	mkdir -p gh-pages
	./clock.sh gh-pages/clock.svg

# Screenshots of the widget examples
.if ${.MAKE.LEVEL} == 0
WIDGETS != make man-widgets
.endif
SNAPSHOTS = ${WIDGETS:C|(.*)|gh-pages/\1.jpg|}
snapshots: pipeglade ${SNAPSHOTS} Makefile

.for W in ${WIDGETS}
gh-pages/${W:S/$/.jpg/}: widget-examples/${W:S/Gtk//:tl}.ui
	@mkdir -p gh-pages
	@if test -e "${.ALLSRC:R}.txt"; then \
		echo "_:load ${.ALLSRC:R}.txt"; \
	else \
		echo "main:snapshot ${.TARGET:R}.svg"; echo "_:main_quit"; \
	fi | ./pipeglade -u ${.ALLSRC} >/dev/null
	convert "${.TARGET:R}.svg" -resize 90% -frame 2 "${.TARGET}" && rm "${.TARGET:R}.svg"
.endfor

gh-pages/gallery.html: ${SNAPSHOTS}
	for i in ${.ALLSRC:T:R}; do \
		echo "<p>"; \
		echo "  <div class=\"display\">"; \
		echo "    <i class=\link-sec\">"; \
		echo -n "      <a href=\"pipeglade.1.html#"; \
		xmllint --html --xpath "string(//div[@class=\"section\"]/h1[text()=\"WIDGETS\"]/../div[@class=\"subsection\"]/h2[text()=\"$${i}\"]/@id)" gh-pages/pipeglade.1.html; \
		echo -n "\">$${i}</a>"; \
		echo "    </i>"; \
		echo "    (cf.&nbsp;<a class=\"link-ext\" href=\"https://developer.gnome.org/gtk3/stable/$${i}.html\">GTK+ 3 Reference Manual</a>)"; \
		echo "    <br>"; \
		echo "    <img src=\"$${i}.jpg\">"; \
		echo "  </div>"; \
		echo "</p>"; \
	done > ${.TARGET}


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
