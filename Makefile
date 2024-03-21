include wg21/Makefile

override SLIDES_DIR := ./slides
override SLIDES_OUT_DIR := $(OUTDIR)/slides

.PHONY: slides
slides:
	rm -rf "$(SLIDES_OUT_DIR)"
	mkdir -p "$(SLIDES_OUT_DIR)"
	cp -R "$(SLIDES_DIR)" "$(OUTDIR)"
