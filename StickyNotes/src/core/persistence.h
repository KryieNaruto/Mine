#pragma once
#include <QString>
#include "core/note_store.h"

bool saveStore(const NoteStore& store, const QString& path);
bool loadStore(NoteStore& store, const QString& path);
