.DEFAULT_GOAL := help

CONTAINER ?=
APPIMAGE_BUILD_DIR ?= build-appimage
APPIMAGE_OUTPUT_DIR ?= $(APPIMAGE_BUILD_DIR)/dist
JOBS ?= 4

.PHONY: help appimage
help:
	@echo 'make appimage                         Build an AppImage with local dependencies'
	@echo 'make appimage CONTAINER=melonds-gpu   Build inside the existing Distrobox container'
	@echo 'Optional: JOBS=4 APPIMAGE_BUILD_DIR=build-appimage APPIMAGE_OUTPUT_DIR=build-appimage/dist'

appimage:
	@if [ -n "$(CONTAINER)" ]; then \
		distrobox enter "$(CONTAINER)" -- bash tools/build-appimage.sh "$(APPIMAGE_BUILD_DIR)" "$(APPIMAGE_OUTPUT_DIR)" "$(JOBS)"; \
	else \
		bash tools/build-appimage.sh "$(APPIMAGE_BUILD_DIR)" "$(APPIMAGE_OUTPUT_DIR)" "$(JOBS)"; \
	fi
