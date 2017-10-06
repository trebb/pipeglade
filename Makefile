# Copyright (c) 2014-2017 Bert Burgemeister <trebbu@googlemail.com>

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
NEWS_VERSION != awk '/^[0-9]+\.[0-9]+\.[0-9]+ .*([0-9]+-[01][0-9]-[0-3][0-9])/{print $$1}' NEWS | head -n1
NEWS_DATE != awk '/^[0-9]+\.[0-9]+\.[0-9]+ .*([0-9]+-[01][0-9]-[0-3][0-9])/{print substr($$2, 2, 10)}' NEWS | head -n1
TODAY != date +%F
MANPAGE_DATE != grep "^\.Dd " pipeglade.1
MANPAGE_TODAY != date '+.Dd %B %e, %Y' | awk '{print $$1, $$2, $$3, $$4}'
.SUFFIXES: .ui .svg .jpg


# Create a new git tag only if there is a NEWS headline in the format
# 1.2.3 (2015-03-22)

# where 1.2.3 matches the current pipeglade version and the date is of
# today, and if pipeglade.1 has today's date in its .Dd line.
# (NEWS headlines are lines that start at column 0.)
git-tag:
	@if test "$(NEWS_DATE)" != "$(TODAY)"; then \
		echo "NEWS: $(NEWS_DATE) != $(TODAY)"; false; \
	fi
	@if test "$(NEWS_VERSION)" != "$(CODE_VERSION)"; then \
		echo "NEWS: $(NEWS_VERSION) != $(CODE_VERSION)"; false; \
	fi
	@if test "$(MANPAGE_DATE)" != "$(MANPAGE_TODAY)"; then \
		echo "MANPAGE: $(MANPAGE_DATE) != $(MANPAGE_TODAY)"; false; \
	fi
	git tag $(CODE_VERSION);


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

# Extract from manual page an outline of the headings
man-outline: www-template/outline.xsl gh-pages/pipeglade.1.html
	@xsltproc --html ${.ALLSRC}

# Our collection of simple test widgets
examples-list:
	@ls -1 widget-examples | grep "\.ui$$"

# Items marked done in list of all widgets
done-list:
	@awk '!/^#/&&/Gtk[A-Z][a-zA-Z]/&&/done/{print $$1}' buildables.txt


# Prepare the www directory

gh-pages:
	$(MAKE) -f Makefile.publish gh-pages

publish:
	$(MAKE) -f Makefile.publish publish
