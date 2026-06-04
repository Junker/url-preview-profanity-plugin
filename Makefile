PLUGIN_NAME = url_preview
SRC = url_preview.c
BUILD_DIR = build
INSTALL_DIR = $(HOME)/.local/share/profanity/plugins

CC ?= gcc
CFLAGS ?= -Wall -Wextra
PROFANITY_LIBS = "-lprofanity"
STROPHE_CFLAGS = $(shell pkg-config --cflags libstrophe 2>/dev/null || echo "")
STROPHE_LIBS = $(shell pkg-config --libs libstrophe 2>/dev/null || echo "-lstrophe")
GLIB_CFLAGS = $(shell pkg-config --cflags glib-2.0 2>/dev/null || echo "")
GLIB_LIBS = $(shell pkg-config --libs glib-2.0 2>/dev/null || echo "-lglib-2.0")

all: $(BUILD_DIR)/$(PLUGIN_NAME).so

$(BUILD_DIR)/$(PLUGIN_NAME).so: $(SRC)
	mkdir -p $(BUILD_DIR)
	$(CC) -shared -o $@ -fPIC $(CFLAGS) $(STROPHE_CFLAGS) $(GLIB_CFLAGS) -Wl,-rpath=$(LIBRARY_PATH) $< $(PROFANITY_LIBS) $(STROPHE_LIBS) $(GLIB_LIBS)

install: all
	mkdir -p $(INSTALL_DIR)
	cp $(BUILD_DIR)/$(PLUGIN_NAME).so $(INSTALL_DIR)/

clean:
	rm -rf $(BUILD_DIR)

.PHONY: all install clean
