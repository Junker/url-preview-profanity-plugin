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
GIO_CFLAGS = $(shell pkg-config --cflags gio-2.0 2>/dev/null || echo "")
GIO_LIBS = $(shell pkg-config --libs gio-2.0 2>/dev/null || echo "-lgio-2.0")
CURL_CFLAGS = $(shell pkg-config --cflags libcurl 2>/dev/null || echo "")
CURL_LIBS = $(shell pkg-config --libs libcurl 2>/dev/null || echo "-lcurl")

all: $(BUILD_DIR)/$(PLUGIN_NAME).so

$(BUILD_DIR)/$(PLUGIN_NAME).so: $(SRC)
	mkdir -p $(BUILD_DIR)
	$(CC) -shared -o $@ -fPIC $(CFLAGS) $(STROPHE_CFLAGS) $(GLIB_CFLAGS) $(GIO_CFLAGS) $(CURL_CFLAGS) -Wl,-rpath=$(LIBRARY_PATH) $< $(PROFANITY_LIBS) $(STROPHE_LIBS) $(GLIB_LIBS) $(GIO_LIBS) $(CURL_LIBS)

test: $(BUILD_DIR)/test_url_preview
	$(BUILD_DIR)/test_url_preview

$(BUILD_DIR)/test_url_preview: test_url_preview.c $(SRC)
	mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ -fPIC $(STROPHE_CFLAGS) $(GLIB_CFLAGS) $(GIO_CFLAGS) $(CURL_CFLAGS) -Wl,-rpath=$(LIBRARY_PATH) $< $(STROPHE_LIBS) $(GLIB_LIBS) $(GIO_LIBS) $(CURL_LIBS)

install: all
	mkdir -p $(INSTALL_DIR)
	cp $(BUILD_DIR)/$(PLUGIN_NAME).so $(INSTALL_DIR)/

clean:
	rm -rf $(BUILD_DIR)

.PHONY: all test install clean
