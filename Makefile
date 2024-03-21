include wg21/Makefile

.PHONY: deploy
deploy: $(PDF)
	rm -rf "./generated/slides"
	mkdir -p "./generated/slides"
	cp -R ./slides/* "./generated/slides/"
