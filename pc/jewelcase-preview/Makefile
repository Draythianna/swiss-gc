# Preview of the jewel case games view (cube/swiss/source/gui/jewelcase.c) on a PC
SRC := ../../cube/swiss/source

preview: preview_probe.c host_main.c gx_soft.c $(SRC)/gui/jewelcase.c $(SRC)/gui/pngdec.c $(wildcard shim/*.h)
	$(CC) -O2 -g -Wall -Wno-unused-function -Wno-misleading-indentation -Ishim -I$(SRC)/gui -o $@ preview_probe.c host_main.c gx_soft.c $(SRC)/gui/pngdec.c -lz -lm

clean:
	rm -f preview

.PHONY: clean
