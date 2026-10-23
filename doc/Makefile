# make -C doc        builds the HTML documentation into doc/_build/html
# make -C doc venv   sets up the Python environment it needs, once
#
# The venv lives outside the source tree (VENV), so it needs no symlinks here.

VENV ?= $(HOME)/.local/share/venvs/awl-doc
SPHINXBUILD = $(VENV)/bin/sphinx-build
SPHINXOPTS ?=

html:
	$(SPHINXBUILD) -b html $(SPHINXOPTS) . _build/html

venv:
	python3 -m venv $(VENV)
	$(VENV)/bin/pip install -r requirements.txt

clean:
	rm -rf _build

.PHONY: html venv clean
