/* Copyright (c) 2013-2015 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "gba/renderers/software-private.h"

#include <mgba/core/tile-cache.h>
#include <mgba/internal/arm/macros.h>
#include <mgba/internal/gba/io.h>

#include <mgba-util/arm-algo.h>
#include <mgba-util/memory.h>

static void GBAVideoSoftwareRendererInit(struct GBAVideoRenderer* renderer);
static void GBAVideoSoftwareRendererDeinit(struct GBAVideoRenderer* renderer);
static void GBAVideoSoftwareRendererReset(struct GBAVideoRenderer* renderer);
static void GBAVideoSoftwareRendererWriteVRAM(struct GBAVideoRenderer* renderer, uint32_t address);
static void GBAVideoSoftwareRendererWriteOAM(struct GBAVideoRenderer* renderer, uint32_t oam);
static void GBAVideoSoftwareRendererWritePalette(struct GBAVideoRenderer* renderer, uint32_t address, uint16_t value);
static uint16_t GBAVideoSoftwareRendererWriteVideoRegister(struct GBAVideoRenderer* renderer, uint32_t address, uint16_t value);
static void GBAVideoSoftwareRendererDrawScanline(struct GBAVideoRenderer* renderer, int y);
static void GBAVideoSoftwareRendererFinishFrame(struct GBAVideoRenderer* renderer);
static void GBAVideoSoftwareRendererGetPixels(struct GBAVideoRenderer* renderer, size_t* stride, const void** pixels);
static void GBAVideoSoftwareRendererPutPixels(struct GBAVideoRenderer* renderer, size_t stride, const void* pixels);

static void GBAVideoSoftwareRendererUpdateDISPCNT(struct GBAVideoSoftwareRenderer* renderer);
static void GBAVideoSoftwareRendererWriteBGCNT(struct GBAVideoSoftwareRenderer* renderer, struct GBAVideoSoftwareBackground* bg, uint16_t value);
static void GBAVideoSoftwareRendererWriteBGPA(struct GBAVideoSoftwareBackground* bg, uint16_t value);
static void GBAVideoSoftwareRendererWriteBGPB(struct GBAVideoSoftwareBackground* bg, uint16_t value);
static void GBAVideoSoftwareRendererWriteBGPC(struct GBAVideoSoftwareBackground* bg, uint16_t value);
static void GBAVideoSoftwareRendererWriteBGPD(struct GBAVideoSoftwareBackground* bg, uint16_t value);
static void GBAVideoSoftwareRendererWriteBGX_LO(struct GBAVideoSoftwareBackground* bg, uint16_t value);
static void GBAVideoSoftwareRendererWriteBGX_HI(struct GBAVideoSoftwareBackground* bg, uint16_t value);
static void GBAVideoSoftwareRendererWriteBGY_LO(struct GBAVideoSoftwareBackground* bg, uint16_t value);
static void GBAVideoSoftwareRendererWriteBGY_HI(struct GBAVideoSoftwareBackground* bg, uint16_t value);
static void GBAVideoSoftwareRendererWriteBLDCNT(struct GBAVideoSoftwareRenderer* renderer, uint16_t value);

static void _cleanOAM(struct GBAVideoSoftwareRenderer* renderer);

static void _updatePalettes(struct GBAVideoSoftwareRenderer* renderer);

static void _breakWindow(struct GBAVideoSoftwareRenderer* softwareRenderer, struct WindowN* win, int y);
static void _breakWindowInner(struct GBAVideoSoftwareRenderer* softwareRenderer, struct WindowN* win);

void GBAVideoSoftwareRendererCreate(struct GBAVideoSoftwareRenderer* renderer) {
	renderer->d.init = GBAVideoSoftwareRendererInit;
	renderer->d.reset = GBAVideoSoftwareRendererReset;
	renderer->d.deinit = GBAVideoSoftwareRendererDeinit;
	renderer->d.writeVideoRegister = GBAVideoSoftwareRendererWriteVideoRegister;
	renderer->d.writeVRAM = GBAVideoSoftwareRendererWriteVRAM;
	renderer->d.writeOAM = GBAVideoSoftwareRendererWriteOAM;
	renderer->d.writePalette = GBAVideoSoftwareRendererWritePalette;
	renderer->d.drawScanline = GBAVideoSoftwareRendererDrawScanline;
	renderer->d.finishFrame = GBAVideoSoftwareRendererFinishFrame;
	renderer->d.getPixels = GBAVideoSoftwareRendererGetPixels;
	renderer->d.putPixels = GBAVideoSoftwareRendererPutPixels;

	renderer->d.disableBG[0] = false;
	renderer->d.disableBG[1] = false;
	renderer->d.disableBG[2] = false;
	renderer->d.disableBG[3] = false;
	renderer->d.disableOBJ = false;
	renderer->tileStride = 0x20;
	renderer->masterEnd = VIDEO_HORIZONTAL_PIXELS;
	renderer->masterHeight = VIDEO_VERTICAL_PIXELS;
	renderer->masterScanlines = VIDEO_VERTICAL_TOTAL_PIXELS;

	renderer->temporaryBuffer = 0;
}

static void GBAVideoSoftwareRendererInit(struct GBAVideoRenderer* renderer) {
	GBAVideoSoftwareRendererReset(renderer);

	struct GBAVideoSoftwareRenderer* softwareRenderer = (struct GBAVideoSoftwareRenderer*) renderer;

	int y;
	for (y = 0; y < softwareRenderer->masterEnd; ++y) {
		color_t* row = &softwareRenderer->outputBuffer[softwareRenderer->outputBufferStride * y];
		int x;
		for (x = 0; x < softwareRenderer->masterEnd; ++x) {
			row[x] = GBA_COLOR_WHITE;
		}
	}
}

static void GBAVideoSoftwareRendererReset(struct GBAVideoRenderer* renderer) {
	struct GBAVideoSoftwareRenderer* softwareRenderer = (struct GBAVideoSoftwareRenderer*) renderer;
	int i;

	softwareRenderer->dispcnt = 0x0080;

	softwareRenderer->target1Obj = 0;
	softwareRenderer->target1Bd = 0;
	softwareRenderer->target2Obj = 0;
	softwareRenderer->target2Bd = 0;
	softwareRenderer->blendEffect = BLEND_NONE;
	for (i = 0; i < 1024; i += 2) {
		uint16_t entry;
		LOAD_16(entry, i, softwareRenderer->d.palette);
		GBAVideoSoftwareRendererWritePalette(renderer, i, entry);
	}
	softwareRenderer->objExtPalette = NULL;
	softwareRenderer->objExtVariantPalette = NULL;
	_updatePalettes(softwareRenderer);

	softwareRenderer->blda = 0;
	softwareRenderer->bldb = 0;
	softwareRenderer->bldy = 0;

	softwareRenderer->winN[0] = (struct WindowN) { .control = { .priority = 0 } };
	softwareRenderer->winN[1] = (struct WindowN) { .control = { .priority = 1 } };
	softwareRenderer->objwin = (struct WindowControl) { .priority = 2 };
	softwareRenderer->winout = (struct WindowControl) { .priority = 3 };
	softwareRenderer->oamMax = 0;

	softwareRenderer->mosaic = 0;

	for (i = 0; i < 4; ++i) {
		struct GBAVideoSoftwareBackground* bg = &softwareRenderer->bg[i];
		bg->index = i;
		bg->enabled = 0;
		bg->priority = 0;
		bg->charBase = 0;
		bg->mosaic = 0;
		bg->multipalette = 0;
		bg->screenBase = 0;
		bg->overflow = 0;
		bg->size = 0;
		bg->target1 = 0;
		bg->target2 = 0;
		bg->x = 0;
		bg->y = 0;
		bg->refx = 0;
		bg->refy = 0;
		bg->dx = 256;
		bg->dmx = 0;
		bg->dy = 0;
		bg->dmy = 256;
		bg->sx = 0;
		bg->sy = 0;
		bg->extPalette = NULL;
		bg->variantPalette = NULL;
	}
}

static void GBAVideoSoftwareRendererDeinit(struct GBAVideoRenderer* renderer) {
	struct GBAVideoSoftwareRenderer* softwareRenderer = (struct GBAVideoSoftwareRenderer*) renderer;
	UNUSED(softwareRenderer);
}

static uint16_t GBAVideoSoftwareRendererWriteVideoRegister(struct GBAVideoRenderer* renderer, uint32_t address, uint16_t value) {
	struct GBAVideoSoftwareRenderer* softwareRenderer = (struct GBAVideoSoftwareRenderer*) renderer;
	switch (address) {
	case REG_DISPCNT:
		softwareRenderer->dispcnt = value;
		GBAVideoSoftwareRendererUpdateDISPCNT(softwareRenderer);
		break;
	case REG_BG0CNT:
		value &= 0xDFFF;
		GBAVideoSoftwareRendererWriteBGCNT(softwareRenderer, &softwareRenderer->bg[0], value);
		break;
	case REG_BG1CNT:
		value &= 0xDFFF;
		GBAVideoSoftwareRendererWriteBGCNT(softwareRenderer, &softwareRenderer->bg[1], value);
		break;
	case REG_BG2CNT:
		value &= 0xFFFF;
		GBAVideoSoftwareRendererWriteBGCNT(softwareRenderer, &softwareRenderer->bg[2], value);
		break;
	case REG_BG3CNT:
		value &= 0xFFFF;
		GBAVideoSoftwareRendererWriteBGCNT(softwareRenderer, &softwareRenderer->bg[3], value);
		break;
	case REG_BG0HOFS:
		value &= 0x01FF;
		softwareRenderer->bg[0].x = value;
		break;
	case REG_BG0VOFS:
		value &= 0x01FF;
		softwareRenderer->bg[0].y = value;
		break;
	case REG_BG1HOFS:
		value &= 0x01FF;
		softwareRenderer->bg[1].x = value;
		break;
	case REG_BG1VOFS:
		value &= 0x01FF;
		softwareRenderer->bg[1].y = value;
		break;
	case REG_BG2HOFS:
		value &= 0x01FF;
		softwareRenderer->bg[2].x = value;
		break;
	case REG_BG2VOFS:
		value &= 0x01FF;
		softwareRenderer->bg[2].y = value;
		break;
	case REG_BG3HOFS:
		value &= 0x01FF;
		softwareRenderer->bg[3].x = value;
		break;
	case REG_BG3VOFS:
		value &= 0x01FF;
		softwareRenderer->bg[3].y = value;
		break;
	case REG_BG2PA:
		GBAVideoSoftwareRendererWriteBGPA(&softwareRenderer->bg[2], value);
		break;
	case REG_BG2PB:
		GBAVideoSoftwareRendererWriteBGPB(&softwareRenderer->bg[2], value);
		break;
	case REG_BG2PC:
		GBAVideoSoftwareRendererWriteBGPC(&softwareRenderer->bg[2], value);
		break;
	case REG_BG2PD:
		GBAVideoSoftwareRendererWriteBGPD(&softwareRenderer->bg[2], value);
		break;
	case REG_BG2X_LO:
		GBAVideoSoftwareRendererWriteBGX_LO(&softwareRenderer->bg[2], value);
		break;
	case REG_BG2X_HI:
		GBAVideoSoftwareRendererWriteBGX_HI(&softwareRenderer->bg[2], value);
		break;
	case REG_BG2Y_LO:
		GBAVideoSoftwareRendererWriteBGY_LO(&softwareRenderer->bg[2], value);
		break;
	case REG_BG2Y_HI:
		GBAVideoSoftwareRendererWriteBGY_HI(&softwareRenderer->bg[2], value);
		break;
	case REG_BG3PA:
		GBAVideoSoftwareRendererWriteBGPA(&softwareRenderer->bg[3], value);
		break;
	case REG_BG3PB:
		GBAVideoSoftwareRendererWriteBGPB(&softwareRenderer->bg[3], value);
		break;
	case REG_BG3PC:
		GBAVideoSoftwareRendererWriteBGPC(&softwareRenderer->bg[3], value);
		break;
	case REG_BG3PD:
		GBAVideoSoftwareRendererWriteBGPD(&softwareRenderer->bg[3], value);
		break;
	case REG_BG3X_LO:
		GBAVideoSoftwareRendererWriteBGX_LO(&softwareRenderer->bg[3], value);
		break;
	case REG_BG3X_HI:
		GBAVideoSoftwareRendererWriteBGX_HI(&softwareRenderer->bg[3], value);
		break;
	case REG_BG3Y_LO:
		GBAVideoSoftwareRendererWriteBGY_LO(&softwareRenderer->bg[3], value);
		break;
	case REG_BG3Y_HI:
		GBAVideoSoftwareRendererWriteBGY_HI(&softwareRenderer->bg[3], value);
		break;
	case REG_BLDCNT:
		GBAVideoSoftwareRendererWriteBLDCNT(softwareRenderer, value);
		value &= 0x3FFF;
		break;
	case REG_BLDALPHA:
		softwareRenderer->blda = value & 0x1F;
		if (softwareRenderer->blda > 0x10) {
			softwareRenderer->blda = 0x10;
		}
		softwareRenderer->bldb = (value >> 8) & 0x1F;
		if (softwareRenderer->bldb > 0x10) {
			softwareRenderer->bldb = 0x10;
		}
		value &= 0x1F1F;
		break;
	case REG_BLDY:
		softwareRenderer->bldy = value & 0x1F;
		if (softwareRenderer->bldy > 0x10) {
			softwareRenderer->bldy = 0x10;
		}
		_updatePalettes(softwareRenderer);
		break;
	case REG_WIN0H:
		softwareRenderer->winN[0].h.end = value;
		softwareRenderer->winN[0].h.start = value >> 8;
		if (softwareRenderer->winN[0].h.start > softwareRenderer->masterEnd && softwareRenderer->winN[0].h.start > softwareRenderer->winN[0].h.end) {
			softwareRenderer->winN[0].h.start = 0;
		}
		if (softwareRenderer->winN[0].h.end > softwareRenderer->masterEnd) {
			softwareRenderer->winN[0].h.end = softwareRenderer->masterEnd;
			if (softwareRenderer->winN[0].h.start > softwareRenderer->masterEnd) {
				softwareRenderer->winN[0].h.start = softwareRenderer->masterEnd;
			}
		}
		break;
	case REG_WIN1H:
		softwareRenderer->winN[1].h.end = value;
		softwareRenderer->winN[1].h.start = value >> 8;
		if (softwareRenderer->winN[1].h.start > softwareRenderer->masterEnd && softwareRenderer->winN[1].h.start > softwareRenderer->winN[1].h.end) {
			softwareRenderer->winN[1].h.start = 0;
		}
		if (softwareRenderer->winN[1].h.end > softwareRenderer->masterEnd) {
			softwareRenderer->winN[1].h.end = softwareRenderer->masterEnd;
			if (softwareRenderer->winN[1].h.start > softwareRenderer->masterEnd) {
				softwareRenderer->winN[1].h.start = softwareRenderer->masterEnd;
			}
		}
		break;
	case REG_WIN0V:
		softwareRenderer->winN[0].v.end = value;
		softwareRenderer->winN[0].v.start = value >> 8;
		if (softwareRenderer->winN[0].v.start > VIDEO_VERTICAL_PIXELS && softwareRenderer->winN[0].v.start > softwareRenderer->winN[0].v.end) {
			softwareRenderer->winN[0].v.start = 0;
		}
		if (softwareRenderer->winN[0].v.end > VIDEO_VERTICAL_PIXELS) {
			softwareRenderer->winN[0].v.end = VIDEO_VERTICAL_PIXELS;
			if (softwareRenderer->winN[0].v.start > VIDEO_VERTICAL_PIXELS) {
				softwareRenderer->winN[0].v.start = VIDEO_VERTICAL_PIXELS;
			}
		}
		break;
	case REG_WIN1V:
		softwareRenderer->winN[1].v.end = value;
		softwareRenderer->winN[1].v.start = value >> 8;
		if (softwareRenderer->winN[1].v.start > VIDEO_VERTICAL_PIXELS && softwareRenderer->winN[1].v.start > softwareRenderer->winN[1].v.end) {
			softwareRenderer->winN[1].v.start = 0;
		}
		if (softwareRenderer->winN[1].v.end > VIDEO_VERTICAL_PIXELS) {
			softwareRenderer->winN[1].v.end = VIDEO_VERTICAL_PIXELS;
			if (softwareRenderer->winN[1].v.start > VIDEO_VERTICAL_PIXELS) {
				softwareRenderer->winN[1].v.start = VIDEO_VERTICAL_PIXELS;
			}
		}
		break;
	case REG_WININ:
		value &= 0x3F3F;
		softwareRenderer->winN[0].control.packed = value;
		softwareRenderer->winN[1].control.packed = value >> 8;
		break;
	case REG_WINOUT:
		value &= 0x3F3F;
		softwareRenderer->winout.packed = value;
		softwareRenderer->objwin.packed = value >> 8;
		break;
	case REG_MOSAIC:
		softwareRenderer->mosaic = value;
		break;
	case REG_GREENSWP:
		mLOG(GBA_VIDEO, STUB, "Stub video register write: 0x%03X", address);
		break;
	default:
		mLOG(GBA_VIDEO, GAME_ERROR, "Invalid video register: 0x%03X", address);
	}
	return value;
}

static void GBAVideoSoftwareRendererWriteVRAM(struct GBAVideoRenderer* renderer, uint32_t address) {
	if (renderer->cache) {
		mTileCacheWriteVRAM(renderer->cache, address);
	}
}

static void GBAVideoSoftwareRendererWriteOAM(struct GBAVideoRenderer* renderer, uint32_t oam) {
	struct GBAVideoSoftwareRenderer* softwareRenderer = (struct GBAVideoSoftwareRenderer*) renderer;
	softwareRenderer->oamDirty = 1;
	UNUSED(oam);
}

static void GBAVideoSoftwareRendererWritePalette(struct GBAVideoRenderer* renderer, uint32_t address, uint16_t value) {
	struct GBAVideoSoftwareRenderer* softwareRenderer = (struct GBAVideoSoftwareRenderer*) renderer;
#ifdef COLOR_16_BIT
#ifdef COLOR_5_6_5
	unsigned color = 0;
	color |= (value & 0x001F) << 11;
	color |= (value & 0x03E0) << 1;
	color |= (value & 0x7C00) >> 10;
#else
	unsigned color = value;
#endif
#else
	unsigned color = 0;
	color |= (value << 3) & 0xF8;
	color |= (value << 6) & 0xF800;
	color |= (value << 9) & 0xF80000;
	color |= (color >> 5) & 0x070707;
#endif
	softwareRenderer->normalPalette[address >> 1] = color;
	if (softwareRenderer->blendEffect == BLEND_BRIGHTEN) {
		softwareRenderer->variantPalette[address >> 1] = _brighten(color, softwareRenderer->bldy);
	} else if (softwareRenderer->blendEffect == BLEND_DARKEN) {
		softwareRenderer->variantPalette[address >> 1] = _darken(color, softwareRenderer->bldy);
	}
	if (renderer->cache) {
		mTileCacheWritePalette(renderer->cache, address);
	}
}

static void _breakWindow(struct GBAVideoSoftwareRenderer* softwareRenderer, struct WindowN* win, int y) {
	if (win->v.end >= win->v.start) {
		if (y >= win->v.end) {
			return;
		}
		if (y < win->v.start) {
			return;
		}
	} else if (y >= win->v.end && y < win->v.start) {
		return;
	}
	if (win->h.end > softwareRenderer->masterEnd || win->h.end < win->h.start) {
		struct WindowN splits[2] = { *win, *win };
		splits[0].h.start = 0;
		splits[1].h.end = softwareRenderer->masterEnd;
		_breakWindowInner(softwareRenderer, &splits[0]);
		_breakWindowInner(softwareRenderer, &splits[1]);
	} else {
		_breakWindowInner(softwareRenderer, win);
	}
}

static void _breakWindowInner(struct GBAVideoSoftwareRenderer* softwareRenderer, struct WindowN* win) {
	int activeWindow;
	int startX = 0;
	if (win->h.end > 0) {
		for (activeWindow = 0; activeWindow < softwareRenderer->nWindows; ++activeWindow) {
			if (win->h.start < softwareRenderer->windows[activeWindow].endX) {
				// Insert a window before the end of the active window
				struct Window oldWindow = softwareRenderer->windows[activeWindow];
				if (win->h.start > startX) {
					// And after the start of the active window
					int nextWindow = softwareRenderer->nWindows;
					++softwareRenderer->nWindows;
					for (; nextWindow > activeWindow; --nextWindow) {
						softwareRenderer->windows[nextWindow] = softwareRenderer->windows[nextWindow - 1];
					}
					softwareRenderer->windows[activeWindow].endX = win->h.start;
					++activeWindow;
				}
				softwareRenderer->windows[activeWindow].control = win->control;
				softwareRenderer->windows[activeWindow].endX = win->h.end;
				if (win->h.end >= oldWindow.endX) {
					// Trim off extra windows we've overwritten
					for (++activeWindow; softwareRenderer->nWindows > activeWindow + 1 && win->h.end >= softwareRenderer->windows[activeWindow].endX; ++activeWindow) {
						if (VIDEO_CHECKS && activeWindow >= MAX_WINDOW) {
							mLOG(GBA_VIDEO, FATAL, "Out of bounds window write will occur");
							return;
						}
						softwareRenderer->windows[activeWindow] = softwareRenderer->windows[activeWindow + 1];
						--softwareRenderer->nWindows;
					}
				} else {
					++activeWindow;
					int nextWindow = softwareRenderer->nWindows;
					++softwareRenderer->nWindows;
					for (; nextWindow > activeWindow; --nextWindow) {
						softwareRenderer->windows[nextWindow] = softwareRenderer->windows[nextWindow - 1];
					}
					softwareRenderer->windows[activeWindow] = oldWindow;
				}
				break;
			}
			startX = softwareRenderer->windows[activeWindow].endX;
		}
	}
#ifdef DEBUG
	if (softwareRenderer->nWindows > MAX_WINDOW) {
		mLOG(GBA_VIDEO, FATAL, "Out of bounds window write occurred!");
	}
#endif
}

static void _cleanOAM(struct GBAVideoSoftwareRenderer* renderer) {
	int i;
	int oamMax = 0;
	for (i = 0; i < 128; ++i) {
		struct GBAObj obj;
		LOAD_16(obj.a, 0, &renderer->d.oam->obj[i].a);
		LOAD_16(obj.b, 0, &renderer->d.oam->obj[i].b);
		LOAD_16(obj.c, 0, &renderer->d.oam->obj[i].c);
		if (GBAObjAttributesAIsTransformed(obj.a) || !GBAObjAttributesAIsDisable(obj.a)) {
			int height = GBAVideoObjSizes[GBAObjAttributesAGetShape(obj.a) * 4 + GBAObjAttributesBGetSize(obj.b)][1];
			if (GBAObjAttributesAIsTransformed(obj.a)) {
				height <<= GBAObjAttributesAGetDoubleSize(obj.a);
			}
			if (GBAObjAttributesAGetY(obj.a) < renderer->masterHeight || GBAObjAttributesAGetY(obj.a) + height >= renderer->masterScanlines) {
				renderer->sprites[oamMax].y = GBAObjAttributesAGetY(obj.a);
				renderer->sprites[oamMax].endY = GBAObjAttributesAGetY(obj.a) + height;
				renderer->sprites[oamMax].obj = obj;
				++oamMax;
			}
		}
	}
	renderer->oamMax = oamMax;
	renderer->oamDirty = 0;
}

static void GBAVideoSoftwareRendererDrawScanline(struct GBAVideoRenderer* renderer, int y) {
	struct GBAVideoSoftwareRenderer* softwareRenderer = (struct GBAVideoSoftwareRenderer*) renderer;

	color_t* row = &softwareRenderer->outputBuffer[softwareRenderer->outputBufferStride * y];
	if (GBARegisterDISPCNTIsForcedBlank(softwareRenderer->dispcnt)) {
		int x;
		for (x = 0; x < softwareRenderer->masterEnd; ++x) {
			row[x] = GBA_COLOR_WHITE;
		}
		return;
	}

	uint16_t* objVramBase = softwareRenderer->d.vramOBJ[0];
	if (GBARegisterDISPCNTGetMode(softwareRenderer->dispcnt) >= 3) {
		softwareRenderer->d.vramOBJ[0] = NULL; // OBJ VRAM bottom is blocked in bitmap modes
	}

	GBAVideoSoftwareRendererPreprocessBuffer(softwareRenderer, y);
	int spriteLayers = GBAVideoSoftwareRendererPreprocessSpriteLayer(softwareRenderer, y);
	softwareRenderer->d.vramOBJ[0] = objVramBase;

	int w;
	unsigned priority;
	for (priority = 0; priority < 4; ++priority) {
		softwareRenderer->end = 0;
		for (w = 0; w < softwareRenderer->nWindows; ++w) {
			softwareRenderer->start = softwareRenderer->end;
			softwareRenderer->end = softwareRenderer->windows[w].endX;
			softwareRenderer->currentWindow = softwareRenderer->windows[w].control;
			if (spriteLayers & (1 << priority)) {
				GBAVideoSoftwareRendererPostprocessSprite(softwareRenderer, priority);
			}
			if (TEST_LAYER_ENABLED(0) && GBARegisterDISPCNTGetMode(softwareRenderer->dispcnt) < 2) {
				GBAVideoSoftwareRendererDrawBackgroundMode0(softwareRenderer, &softwareRenderer->bg[0], y);
			}
			if (TEST_LAYER_ENABLED(1) && GBARegisterDISPCNTGetMode(softwareRenderer->dispcnt) < 2) {
				GBAVideoSoftwareRendererDrawBackgroundMode0(softwareRenderer, &softwareRenderer->bg[1], y);
			}
			if (TEST_LAYER_ENABLED(2)) {
				switch (GBARegisterDISPCNTGetMode(softwareRenderer->dispcnt)) {
				case 0:
					GBAVideoSoftwareRendererDrawBackgroundMode0(softwareRenderer, &softwareRenderer->bg[2], y);
					break;
				case 1:
				case 2:
					GBAVideoSoftwareRendererDrawBackgroundMode2(softwareRenderer, &softwareRenderer->bg[2], y);
					break;
				case 3:
					GBAVideoSoftwareRendererDrawBackgroundMode3(softwareRenderer, &softwareRenderer->bg[2], y);
					break;
				case 4:
					GBAVideoSoftwareRendererDrawBackgroundMode4(softwareRenderer, &softwareRenderer->bg[2], y);
					break;
				case 5:
					GBAVideoSoftwareRendererDrawBackgroundMode5(softwareRenderer, &softwareRenderer->bg[2], y);
					break;
				}
			}
			if (TEST_LAYER_ENABLED(3)) {
				switch (GBARegisterDISPCNTGetMode(softwareRenderer->dispcnt)) {
				case 0:
					GBAVideoSoftwareRendererDrawBackgroundMode0(softwareRenderer, &softwareRenderer->bg[3], y);
					break;
				case 2:
					GBAVideoSoftwareRendererDrawBackgroundMode2(softwareRenderer, &softwareRenderer->bg[3], y);
					break;
				}
			}
		}
	}
	softwareRenderer->bg[2].sx += softwareRenderer->bg[2].dmx;
	softwareRenderer->bg[2].sy += softwareRenderer->bg[2].dmy;
	softwareRenderer->bg[3].sx += softwareRenderer->bg[3].dmx;
	softwareRenderer->bg[3].sy += softwareRenderer->bg[3].dmy;

	GBAVideoSoftwareRendererPostprocessBuffer(softwareRenderer);

#ifdef COLOR_16_BIT
#if defined(__ARM_NEON) && !defined(__APPLE__)
	_to16Bit(row, softwareRenderer->row, softwareRenderer->masterEnd);
#else
	for (x = 0; x < softwareRenderer->masterEnd; ++x) {
		row[x] = softwareRenderer->row[x];
	}
#endif
#else
	memcpy(row, softwareRenderer->row, softwareRenderer->masterEnd * sizeof(*row));
#endif
}

static void GBAVideoSoftwareRendererFinishFrame(struct GBAVideoRenderer* renderer) {
	struct GBAVideoSoftwareRenderer* softwareRenderer = (struct GBAVideoSoftwareRenderer*) renderer;

	if (softwareRenderer->temporaryBuffer) {
		mappedMemoryFree(softwareRenderer->temporaryBuffer, softwareRenderer->masterEnd * softwareRenderer->masterHeight * 4);
		softwareRenderer->temporaryBuffer = 0;
	}
	softwareRenderer->bg[2].sx = softwareRenderer->bg[2].refx;
	softwareRenderer->bg[2].sy = softwareRenderer->bg[2].refy;
	softwareRenderer->bg[3].sx = softwareRenderer->bg[3].refx;
	softwareRenderer->bg[3].sy = softwareRenderer->bg[3].refy;
}

static void GBAVideoSoftwareRendererGetPixels(struct GBAVideoRenderer* renderer, size_t* stride, const void** pixels) {
	struct GBAVideoSoftwareRenderer* softwareRenderer = (struct GBAVideoSoftwareRenderer*) renderer;
	*stride = softwareRenderer->outputBufferStride;
	*pixels = softwareRenderer->outputBuffer;
}

static void GBAVideoSoftwareRendererPutPixels(struct GBAVideoRenderer* renderer, size_t stride, const void* pixels) {
	struct GBAVideoSoftwareRenderer* softwareRenderer = (struct GBAVideoSoftwareRenderer*) renderer;

	const color_t* colorPixels = pixels;
	unsigned i;
	for (i = 0; i < softwareRenderer->masterHeight; ++i) {
		memmove(&softwareRenderer->outputBuffer[softwareRenderer->outputBufferStride * i], &colorPixels[stride * i], softwareRenderer->masterEnd * BYTES_PER_PIXEL);
	}
}

static void GBAVideoSoftwareRendererUpdateDISPCNT(struct GBAVideoSoftwareRenderer* renderer) {
	renderer->bg[0].enabled = GBARegisterDISPCNTGetBg0Enable(renderer->dispcnt) && !renderer->d.disableBG[0];
	renderer->bg[1].enabled = GBARegisterDISPCNTGetBg1Enable(renderer->dispcnt) && !renderer->d.disableBG[1];
	renderer->bg[2].enabled = GBARegisterDISPCNTGetBg2Enable(renderer->dispcnt) && !renderer->d.disableBG[2];
	renderer->bg[3].enabled = GBARegisterDISPCNTGetBg3Enable(renderer->dispcnt) && !renderer->d.disableBG[3];
}

static void GBAVideoSoftwareRendererWriteBGCNT(struct GBAVideoSoftwareRenderer* renderer, struct GBAVideoSoftwareBackground* bg, uint16_t value) {
	UNUSED(renderer);
	bg->priority = GBARegisterBGCNTGetPriority(value);
	bg->charBase = GBARegisterBGCNTGetCharBase(value) << 14;
	if (!renderer->d.vramBG[4]) {
		bg->charBase &= 0xC000;
	}
	bg->mosaic = GBARegisterBGCNTGetMosaic(value);
	bg->multipalette = GBARegisterBGCNTGet256Color(value);
	bg->screenBase &= ~0xF800;
	bg->screenBase |= GBARegisterBGCNTGetScreenBase(value) << 11;
	bg->overflow = GBARegisterBGCNTGetOverflow(value);
	bg->size = GBARegisterBGCNTGetSize(value);
	bg->control = value;
}

static void GBAVideoSoftwareRendererWriteBGPA(struct GBAVideoSoftwareBackground* bg, uint16_t value) {
	bg->dx = value;
}

static void GBAVideoSoftwareRendererWriteBGPB(struct GBAVideoSoftwareBackground* bg, uint16_t value) {
	bg->dmx = value;
}

static void GBAVideoSoftwareRendererWriteBGPC(struct GBAVideoSoftwareBackground* bg, uint16_t value) {
	bg->dy = value;
}

static void GBAVideoSoftwareRendererWriteBGPD(struct GBAVideoSoftwareBackground* bg, uint16_t value) {
	bg->dmy = value;
}

static void GBAVideoSoftwareRendererWriteBGX_LO(struct GBAVideoSoftwareBackground* bg, uint16_t value) {
	bg->refx = (bg->refx & 0xFFFF0000) | value;
	bg->sx = bg->refx;
}

static void GBAVideoSoftwareRendererWriteBGX_HI(struct GBAVideoSoftwareBackground* bg, uint16_t value) {
	bg->refx = (bg->refx & 0x0000FFFF) | (value << 16);
	bg->refx <<= 4;
	bg->refx >>= 4;
	bg->sx = bg->refx;
}

static void GBAVideoSoftwareRendererWriteBGY_LO(struct GBAVideoSoftwareBackground* bg, uint16_t value) {
	bg->refy = (bg->refy & 0xFFFF0000) | value;
	bg->sy = bg->refy;
}

static void GBAVideoSoftwareRendererWriteBGY_HI(struct GBAVideoSoftwareBackground* bg, uint16_t value) {
	bg->refy = (bg->refy & 0x0000FFFF) | (value << 16);
	bg->refy <<= 4;
	bg->refy >>= 4;
	bg->sy = bg->refy;
}

static void GBAVideoSoftwareRendererWriteBLDCNT(struct GBAVideoSoftwareRenderer* renderer, uint16_t value) {
	enum BlendEffect oldEffect = renderer->blendEffect;

	renderer->bg[0].target1 = GBARegisterBLDCNTGetTarget1Bg0(value);
	renderer->bg[1].target1 = GBARegisterBLDCNTGetTarget1Bg1(value);
	renderer->bg[2].target1 = GBARegisterBLDCNTGetTarget1Bg2(value);
	renderer->bg[3].target1 = GBARegisterBLDCNTGetTarget1Bg3(value);
	renderer->bg[0].target2 = GBARegisterBLDCNTGetTarget2Bg0(value);
	renderer->bg[1].target2 = GBARegisterBLDCNTGetTarget2Bg1(value);
	renderer->bg[2].target2 = GBARegisterBLDCNTGetTarget2Bg2(value);
	renderer->bg[3].target2 = GBARegisterBLDCNTGetTarget2Bg3(value);

	renderer->blendEffect = GBARegisterBLDCNTGetEffect(value);
	renderer->target1Obj = GBARegisterBLDCNTGetTarget1Obj(value);
	renderer->target1Bd = GBARegisterBLDCNTGetTarget1Bd(value);
	renderer->target2Obj = GBARegisterBLDCNTGetTarget2Obj(value);
	renderer->target2Bd = GBARegisterBLDCNTGetTarget2Bd(value);

	if (oldEffect != renderer->blendEffect) {
		_updatePalettes(renderer);
	}
}

void GBAVideoSoftwareRendererPreprocessBuffer(struct GBAVideoSoftwareRenderer* softwareRenderer, int y) {
	int x;
	int masterEnd = softwareRenderer->masterEnd;
	for (x = 0; x < masterEnd; x += 4) {
		softwareRenderer->spriteLayer[x] = FLAG_UNWRITTEN;
		softwareRenderer->spriteLayer[x + 1] = FLAG_UNWRITTEN;
		softwareRenderer->spriteLayer[x + 2] = FLAG_UNWRITTEN;
		softwareRenderer->spriteLayer[x + 3] = FLAG_UNWRITTEN;
	}

	softwareRenderer->windows[0].endX = softwareRenderer->masterEnd;
	softwareRenderer->nWindows = 1;
	if (GBARegisterDISPCNTIsWin0Enable(softwareRenderer->dispcnt) || GBARegisterDISPCNTIsWin1Enable(softwareRenderer->dispcnt) || GBARegisterDISPCNTIsObjwinEnable(softwareRenderer->dispcnt)) {
		softwareRenderer->windows[0].control = softwareRenderer->winout;
		if (GBARegisterDISPCNTIsWin1Enable(softwareRenderer->dispcnt)) {
			_breakWindow(softwareRenderer, &softwareRenderer->winN[1], y);
		}
		if (GBARegisterDISPCNTIsWin0Enable(softwareRenderer->dispcnt)) {
			_breakWindow(softwareRenderer, &softwareRenderer->winN[0], y);
		}
	} else {
		softwareRenderer->windows[0].control.packed = 0xFF;
	}

	GBAVideoSoftwareRendererUpdateDISPCNT(softwareRenderer);

	int w;
	x = 0;
	for (w = 0; w < softwareRenderer->nWindows; ++w) {
		// TOOD: handle objwin on backdrop
		uint32_t backdrop = FLAG_UNWRITTEN | FLAG_PRIORITY | FLAG_IS_BACKGROUND;
		if (!softwareRenderer->target1Bd || softwareRenderer->blendEffect == BLEND_NONE || softwareRenderer->blendEffect == BLEND_ALPHA || !GBAWindowControlIsBlendEnable(softwareRenderer->windows[w].control.packed)) {
			backdrop |= softwareRenderer->normalPalette[0];
		} else {
			backdrop |= softwareRenderer->variantPalette[0];
		}
		int end = softwareRenderer->windows[w].endX;
		for (; x & 3; ++x) {
			softwareRenderer->row[x] = backdrop;
		}
		for (; x < end - 3; x += 4) {
			softwareRenderer->row[x] = backdrop;
			softwareRenderer->row[x + 1] = backdrop;
			softwareRenderer->row[x + 2] = backdrop;
			softwareRenderer->row[x + 3] = backdrop;
		}
		for (; x < end; ++x) {
			softwareRenderer->row[x] = backdrop;
		}
	}
}

void GBAVideoSoftwareRendererPostprocessBuffer(struct GBAVideoSoftwareRenderer* softwareRenderer) {
	int x, w;
	if (softwareRenderer->target2Bd) {
		x = 0;
		for (w = 0; w < softwareRenderer->nWindows; ++w) {
			uint32_t backdrop = 0;
			if (!softwareRenderer->target1Bd || softwareRenderer->blendEffect == BLEND_NONE || softwareRenderer->blendEffect == BLEND_ALPHA || !GBAWindowControlIsBlendEnable(softwareRenderer->windows[w].control.packed)) {
				backdrop |= softwareRenderer->normalPalette[0];
			} else {
				backdrop |= softwareRenderer->variantPalette[0];
			}
			int end = softwareRenderer->windows[w].endX;
			for (; x < end; ++x) {
				uint32_t color = softwareRenderer->row[x];
				if (color & FLAG_TARGET_1) {
					softwareRenderer->row[x] = _mix(softwareRenderer->bldb, backdrop, softwareRenderer->blda, color);
				}
			}
		}
	}
	if (softwareRenderer->target1Obj && (softwareRenderer->blendEffect == BLEND_DARKEN || softwareRenderer->blendEffect == BLEND_BRIGHTEN)) {
		x = 0;
		for (w = 0; w < softwareRenderer->nWindows; ++w) {
			if (!GBAWindowControlIsBlendEnable(softwareRenderer->windows[w].control.packed)) {
				continue;
			}
			int end = softwareRenderer->windows[w].endX;
			if (softwareRenderer->blendEffect == BLEND_DARKEN) {
				for (; x < end; ++x) {
					uint32_t color = softwareRenderer->row[x];
					if ((color & 0xFF000000) == FLAG_REBLEND) {
						softwareRenderer->row[x] = _darken(color, softwareRenderer->bldy);
					}
				}
			} else if (softwareRenderer->blendEffect == BLEND_BRIGHTEN) {
				for (; x < end; ++x) {
					uint32_t color = softwareRenderer->row[x];
					if ((color & 0xFF000000) == FLAG_REBLEND) {
						softwareRenderer->row[x] = _brighten(color, softwareRenderer->bldy);
					}
				}
			}
		}
	}
}

int GBAVideoSoftwareRendererPreprocessSpriteLayer(struct GBAVideoSoftwareRenderer* renderer, int y) {
	int w;
	renderer->end = 0;
	int spriteLayers = 0;
	if (GBARegisterDISPCNTIsObjEnable(renderer->dispcnt) && !renderer->d.disableOBJ) {
		if (renderer->oamDirty) {
			_cleanOAM(renderer);
		}
		renderer->spriteCyclesRemaining = GBARegisterDISPCNTIsHblankIntervalFree(renderer->dispcnt) ? OBJ_HBLANK_FREE_LENGTH : OBJ_LENGTH;
		int mosaicV = GBAMosaicControlGetObjV(renderer->mosaic) + 1;
		int mosaicY = y - (y % mosaicV);
		for (w = 0; w < renderer->nWindows; ++w) {
			renderer->start = renderer->end;
			renderer->end = renderer->windows[w].endX;
			renderer->currentWindow = renderer->windows[w].control;
			if (!GBAWindowControlIsObjEnable(renderer->currentWindow.packed) && !GBARegisterDISPCNTIsObjwinEnable(renderer->dispcnt)) {
				continue;
			}
			int i;
			int drawn;

			for (i = 0; i < renderer->oamMax; ++i) {
				int localY = y;
				if (renderer->spriteCyclesRemaining <= 0) {
					break;
				}
				struct GBAVideoSoftwareSprite* sprite = &renderer->sprites[i];
				if (GBAObjAttributesAIsMosaic(sprite->obj.a)) {
					localY = mosaicY;
				}
				if ((localY < sprite->y && (sprite->endY - 256 < 0 || localY >= sprite->endY - 256)) || localY >= sprite->endY) {
					continue;
				}
				drawn = GBAVideoSoftwareRendererPreprocessSprite(renderer, &sprite->obj, localY);
				spriteLayers |= drawn << GBAObjAttributesCGetPriority(sprite->obj.c);
			}
		}
	}
	return spriteLayers;
}

static void _updatePalettes(struct GBAVideoSoftwareRenderer* renderer) {
	int i;
	if (renderer->blendEffect == BLEND_BRIGHTEN) {
		for (i = 0; i < 512; ++i) {
			renderer->variantPalette[i] = _brighten(renderer->normalPalette[i], renderer->bldy);
		}
	} else if (renderer->blendEffect == BLEND_DARKEN) {
		for (i = 0; i < 512; ++i) {
			renderer->variantPalette[i] = _darken(renderer->normalPalette[i], renderer->bldy);
		}
	} else {
		for (i = 0; i < 512; ++i) {
			renderer->variantPalette[i] = renderer->normalPalette[i];
		}
	}
}
