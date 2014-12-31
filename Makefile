PREFIX = /usr/local
CCFLAGS = -Wall `pkg-config gtk+-3.0 --cflags` `pkg-config gmodule-2.0 --cflags`
LIBS = `pkg-config gtk+-3.0 --libs` `pkg-config gmodule-2.0 --libs` -lpthread
CC != which cc
RM = rm -f
VERSION != uname

pipeglade: pipeglade.c
	$(CC) $< -o $@ $(CCFLAGS) $(LIBS)

install: pipeglade pipeglade.1
	mkdir -p $(PREFIX)/bin/
	mkdir -p $(PREFIX)/man/man1/
	cp pipeglade $(PREFIX)/bin/
	gzip -c pipeglade.1 > $(PREFIX)/man/man1/pipeglade.1.gz

gh-pages: gh-pages/index.html gh-pages/pipeglade.1.html

gh-pages/index.html gh-pages/pipeglade.1.html: pipeglade.1 html-template/index.html
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
	$(RM) gh-pages/statcounter.html gh-pages/LICENSE

git-tag:
	git tag `awk '/#define VERSION/{print $$3}' pipeglade.c | tr -d '"'`

publish: clean gh-pages
	(cd gh-pages; git init; git add ./; git commit -a -m "gh-pages pseudo commit"; git push git@github.com:trebb/pipeglade.git +master:gh-pages)

clean:
	$(RM) pipeglade
	$(RM) -r gh-pages
