// Aseprite
// Copyright (C) 2001-2016  David Capello
//
// This program is distributed under the terms of
// the End-User License Agreement for Aseprite.

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "app/document.h"

#include "app/app.h"
#include "app/color_target.h"
#include "app/color_utils.h"
#include "app/context.h"
#include "app/document_api.h"
#include "app/document_undo.h"
#include "app/file/format_options.h"
#include "app/flatten.h"
#include "app/pref/preferences.h"
#include "app/util/create_cel_copy.h"
#include "base/memory.h"
#include "base/unique_ptr.h"
#include "doc/cel.h"
#include "doc/context.h"
#include "doc/document_event.h"
#include "doc/document_observer.h"
#include "doc/frame_tag.h"
#include "doc/layer.h"
#include "doc/mask.h"
#include "doc/mask_boundaries.h"
#include "doc/palette.h"
#include "doc/sprite.h"

#include <map>

namespace app {

using namespace base;
using namespace doc;

Document::Document(Sprite* sprite)
  : m_undo(new DocumentUndo)
  , m_associated_to_file(false)
    // Information about the file format used to load/save this document
  , m_format_options(NULL)
  // Mask
  , m_mask(new Mask())
  , m_maskVisible(true)
{
  setFilename("Sprite");

  if (sprite)
    sprites().add(sprite);
}

Document::~Document()
{
  // We cannot be in a context at this moment. If we were in a
  // context, doc::~Document() would remove the document from the
  // context and it would generate onRemoveDocument() notifications,
  // which could result in serious problems for observers expecting a
  // fully created app::Document.
  ASSERT(context() == NULL);
}

DocumentApi Document::getApi(Transaction& transaction)
{
  return DocumentApi(this, transaction);
}

//////////////////////////////////////////////////////////////////////
// Main properties

color_t Document::bgColor() const
{
  return color_utils::color_for_target(
    Preferences::instance().colorBar.bgColor(),
    ColorTarget(ColorTarget::BackgroundLayer,
                sprite()->pixelFormat(),
                sprite()->transparentColor()));
}

color_t Document::bgColor(Layer* layer) const
{
  if (layer->isBackground())
    return color_utils::color_for_layer(
      Preferences::instance().colorBar.bgColor(),
      layer);
  else
    return layer->sprite()->transparentColor();
}

//////////////////////////////////////////////////////////////////////
// Notifications

void Document::notifyGeneralUpdate()
{
  doc::DocumentEvent ev(this);
  notify_observers<doc::DocumentEvent&>(&doc::DocumentObserver::onGeneralUpdate, ev);
}

void Document::notifySpritePixelsModified(Sprite* sprite, const gfx::Region& region, frame_t frame)
{
  doc::DocumentEvent ev(this);
  ev.sprite(sprite);
  ev.region(region);
  ev.frame(frame);
  notify_observers<doc::DocumentEvent&>(&doc::DocumentObserver::onSpritePixelsModified, ev);
}

void Document::notifyExposeSpritePixels(Sprite* sprite, const gfx::Region& region)
{
  doc::DocumentEvent ev(this);
  ev.sprite(sprite);
  ev.region(region);
  notify_observers<doc::DocumentEvent&>(&doc::DocumentObserver::onExposeSpritePixels, ev);
}

void Document::notifyLayerMergedDown(Layer* srcLayer, Layer* targetLayer)
{
  doc::DocumentEvent ev(this);
  ev.sprite(srcLayer->sprite());
  ev.layer(srcLayer);
  ev.targetLayer(targetLayer);
  notify_observers<doc::DocumentEvent&>(&doc::DocumentObserver::onLayerMergedDown, ev);
}

void Document::notifyCelMoved(Layer* fromLayer, frame_t fromFrame, Layer* toLayer, frame_t toFrame)
{
  doc::DocumentEvent ev(this);
  ev.sprite(fromLayer->sprite());
  ev.layer(fromLayer);
  ev.frame(fromFrame);
  ev.targetLayer(toLayer);
  ev.targetFrame(toFrame);
  notify_observers<doc::DocumentEvent&>(&doc::DocumentObserver::onCelMoved, ev);
}

void Document::notifyCelCopied(Layer* fromLayer, frame_t fromFrame, Layer* toLayer, frame_t toFrame)
{
  doc::DocumentEvent ev(this);
  ev.sprite(fromLayer->sprite());
  ev.layer(fromLayer);
  ev.frame(fromFrame);
  ev.targetLayer(toLayer);
  ev.targetFrame(toFrame);
  notify_observers<doc::DocumentEvent&>(&doc::DocumentObserver::onCelCopied, ev);
}

void Document::notifySelectionChanged()
{
  doc::DocumentEvent ev(this);
  notify_observers<doc::DocumentEvent&>(&doc::DocumentObserver::onSelectionChanged, ev);
}

bool Document::isModified() const
{
  return !m_undo->isSavedState();
}

bool Document::isAssociatedToFile() const
{
  return m_associated_to_file;
}

void Document::markAsSaved()
{
  m_undo->markSavedState();
  m_associated_to_file = true;
}

void Document::impossibleToBackToSavedState()
{
  m_undo->impossibleToBackToSavedState();
}

bool Document::needsBackup() const
{
  // If the undo history isn't empty, the user has modified the
  // document, so we need to backup those changes.
  return m_undo->canUndo() || m_undo->canRedo();
}

//////////////////////////////////////////////////////////////////////
// Loaded options from file

void Document::setFormatOptions(const base::SharedPtr<FormatOptions>& format_options)
{
  m_format_options = format_options;
}

//////////////////////////////////////////////////////////////////////
// Boundaries

void Document::generateMaskBoundaries(const Mask* mask)
{
  m_maskBoundaries.reset();

  // No mask specified? Use the current one in the document
  if (!mask) {
    if (!isMaskVisible())       // The mask is hidden
      return;                   // Done, without boundaries
    else
      mask = this->mask();      // Use the document mask
  }

  ASSERT(mask);

  if (!mask->isEmpty()) {
    m_maskBoundaries.reset(new MaskBoundaries(mask->bitmap()));
    m_maskBoundaries->offset(mask->bounds().x,
                             mask->bounds().y);
  }

  // TODO move this to the exact place where selection is modified.
  notifySelectionChanged();
}

//////////////////////////////////////////////////////////////////////
// Mask

void Document::setMask(const Mask* mask)
{
  m_mask.reset(new Mask(*mask));
  m_maskVisible = true;

  resetTransformation();
}

bool Document::isMaskVisible() const
{
  return
    m_maskVisible &&            // The mask was not hidden by the user explicitly
    m_mask &&                   // The mask does exist
    !m_mask->isEmpty();         // The mask is not empty
}

void Document::setMaskVisible(bool visible)
{
  m_maskVisible = visible;
}

//////////////////////////////////////////////////////////////////////
// Transformation

Transformation Document::getTransformation() const
{
  return m_transformation;
}

void Document::setTransformation(const Transformation& transform)
{
  m_transformation = transform;
}

void Document::resetTransformation()
{
  if (m_mask)
    m_transformation = Transformation(gfx::RectF(m_mask->bounds()));
  else
    m_transformation = Transformation();
}

//////////////////////////////////////////////////////////////////////
// Copying

void Document::copyLayerContent(const Layer* sourceLayer0, Document* destDoc, Layer* destLayer0) const
{
  LayerFlags dstFlags = sourceLayer0->flags();

  // Remove the "background" flag if the destDoc already has a background layer.
  if (((int)dstFlags & (int)LayerFlags::Background) == (int)LayerFlags::Background &&
      (destDoc->sprite()->backgroundLayer())) {
    dstFlags = (LayerFlags)((int)dstFlags & ~(int)(LayerFlags::BackgroundLayerFlags));
  }

  // Copy the layer name/flags/user data
  destLayer0->setName(sourceLayer0->name());
  destLayer0->setFlags(dstFlags);
  destLayer0->setUserData(sourceLayer0->userData());

  if (sourceLayer0->isImage() && destLayer0->isImage()) {
    const LayerImage* sourceLayer = static_cast<const LayerImage*>(sourceLayer0);
    LayerImage* destLayer = static_cast<LayerImage*>(destLayer0);

    // Copy blend mode and opacity
    destLayer->setBlendMode(sourceLayer->blendMode());
    destLayer->setOpacity(sourceLayer->opacity());

    // Copy cels
    CelConstIterator it = sourceLayer->getCelBegin();
    CelConstIterator end = sourceLayer->getCelEnd();

    std::map<ObjectId, Cel*> linked;

    for (; it != end; ++it) {
      const Cel* sourceCel = *it;
      if (sourceCel->frame() > destLayer->sprite()->lastFrame())
        break;

      base::UniquePtr<Cel> newCel;

      auto it = linked.find(sourceCel->data()->id());
      if (it != linked.end()) {
        newCel.reset(Cel::createLink(it->second));
        newCel->setFrame(sourceCel->frame());
      }
      else {
        newCel.reset(create_cel_copy(sourceCel,
                                     destLayer->sprite(),
                                     sourceCel->frame()));
        linked.insert(std::make_pair(sourceCel->data()->id(), newCel.get()));
      }

      destLayer->addCel(newCel);
      newCel.release();
    }
  }
  else if (sourceLayer0->isFolder() && destLayer0->isFolder()) {
    const LayerFolder* sourceLayer = static_cast<const LayerFolder*>(sourceLayer0);
    LayerFolder* destLayer = static_cast<LayerFolder*>(destLayer0);

    LayerConstIterator it = sourceLayer->getLayerBegin();
    LayerConstIterator end = sourceLayer->getLayerEnd();

    for (; it != end; ++it) {
      Layer* sourceChild = *it;
      base::UniquePtr<Layer> destChild(NULL);

      if (sourceChild->isImage()) {
        destChild.reset(new LayerImage(destLayer->sprite()));
        copyLayerContent(sourceChild, destDoc, destChild);
      }
      else if (sourceChild->isFolder()) {
        destChild.reset(new LayerFolder(destLayer->sprite()));
        copyLayerContent(sourceChild, destDoc, destChild);
      }
      else {
        ASSERT(false);
      }

      ASSERT(destChild != NULL);

      // Add the new layer in the sprite.

      Layer* newLayer = destChild.release();
      Layer* afterThis = destLayer->getLastLayer();

      destLayer->addLayer(newLayer);
      destChild.release();

      destLayer->stackLayer(newLayer, afterThis);
    }
  }
  else  {
    ASSERT(false && "Trying to copy two incompatible layers");
  }
}

Document* Document::duplicate(DuplicateType type) const
{
  const Sprite* sourceSprite = sprite();
  base::UniquePtr<Sprite> spriteCopyPtr(new Sprite(
      sourceSprite->pixelFormat(),
      sourceSprite->width(),
      sourceSprite->height(),
      sourceSprite->palette(frame_t(0))->size()));

  base::UniquePtr<Document> documentCopy(new Document(spriteCopyPtr));
  Sprite* spriteCopy = spriteCopyPtr.release();

  spriteCopy->setTotalFrames(sourceSprite->totalFrames());

  // Copy frames duration
  for (frame_t i(0); i < sourceSprite->totalFrames(); ++i)
    spriteCopy->setFrameDuration(i, sourceSprite->frameDuration(i));

  // Copy frame tags
  for (const FrameTag* tag : sourceSprite->frameTags())
    spriteCopy->frameTags().add(new FrameTag(*tag));

  // Copy color palettes
  {
    PalettesList::const_iterator it = sourceSprite->getPalettes().begin();
    PalettesList::const_iterator end = sourceSprite->getPalettes().end();
    for (; it != end; ++it) {
      const Palette* pal = *it;
      spriteCopy->setPalette(pal, true);
    }
  }

  switch (type) {

    case DuplicateExactCopy:
      // Copy the layer folder
      copyLayerContent(sourceSprite->folder(), documentCopy, spriteCopy->folder());

      ASSERT((spriteCopy->backgroundLayer() && sourceSprite->backgroundLayer()) ||
             (!spriteCopy->backgroundLayer() && !sourceSprite->backgroundLayer()));
      break;

    case DuplicateWithFlattenLayers:
      {
        // Flatten layers
        ASSERT(sourceSprite->folder() != NULL);

        LayerImage* flatLayer = create_flatten_layer_copy
            (spriteCopy,
             sourceSprite->folder(),
             gfx::Rect(0, 0, sourceSprite->width(), sourceSprite->height()),
             frame_t(0), sourceSprite->lastFrame());

        // Add and select the new flat layer
        spriteCopy->folder()->addLayer(flatLayer);

        // Configure the layer as background only if the original
        // sprite has a background layer.
        if (sourceSprite->backgroundLayer() != NULL)
          flatLayer->configureAsBackground();
      }
      break;
  }

  documentCopy->setMask(mask());
  documentCopy->m_maskVisible = m_maskVisible;
  documentCopy->generateMaskBoundaries();

  return documentCopy.release();
}

void Document::onContextChanged()
{
  m_undo->setContext(context());
}

} // namespace app
