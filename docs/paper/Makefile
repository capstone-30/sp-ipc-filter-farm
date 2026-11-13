TARGET := main
TEX := pdflatex
BIB := bibtex
FLAGS := -interaction=nonstopmode -halt-on-error

.PHONY: all clean

all: $(TARGET).pdf

$(TARGET).pdf: $(TARGET).tex references.bib
	$(TEX) $(FLAGS) $(TARGET)
	$(BIB) $(TARGET)
	$(TEX) $(FLAGS) $(TARGET)
	$(TEX) $(FLAGS) $(TARGET)

clean:
	rm -f $(TARGET).aux $(TARGET).bbl $(TARGET).blg $(TARGET).log \
	      $(TARGET).out $(TARGET).toc $(TARGET).pdf \
	      $(TARGET).fdb_latexmk $(TARGET).fls $(TARGET).synctex.gz
