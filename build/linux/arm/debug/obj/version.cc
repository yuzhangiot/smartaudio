/* This file is auto-generated.  Do not modify. */
/******************************************************************************
 * Copyright (c) 2013, AllSeen Alliance. All rights reserved.
 *
 *    Permission to use, copy, modify, and/or distribute this software for any
 *    purpose with or without fee is hereby granted, provided that the above
 *    copyright notice and this permission notice appear in all copies.
 *
 *    THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *    WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *    MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *    ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *    WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *    ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *    OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 ******************************************************************************/

#include <alljoyn/audio/Audio.h>
#include <alljoyn/version.h>

static const char product[] = "AllJoyn Audio";
static const unsigned int architecture = 0;
static const unsigned int apiLevel = 0;
static const unsigned int release = 1;

static const char version[] = "v0.00.000";
static const char build[] = "v0.00.000 (Built Thu Nov 19 01:41:59 UTC 2015 by pi - Git: smartaudio.git branch: 'master' tag: '<none>' commit ref: d2d2610eedfb8a66419d02096b7bd2c19a3c73a4)";

const char * ajn::services::audio::GetVersion()
{
    return version;
}

const char * ajn::services::audio::GetBuildInfo()
{
    return build;
}

uint32_t ajn::services::audio::GetNumericVersion()
{
    return GenerateVersionValue(architecture, apiLevel, release);
}
