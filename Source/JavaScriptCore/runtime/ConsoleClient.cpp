/*
 * Copyright (C) 2013-2024 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "ConsoleClient.h"

#include "JSCJSValueInlines.h"
#include "JSGlobalObject.h"
#include "ScriptArguments.h"
#include "ScriptCallStack.h"
#include "ScriptCallStackFactory.h"
#include "TopExceptionScope.h"
#include <wtf/Assertions.h>
#include <wtf/StdLibExtras.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/text/CString.h>
#include <wtf/text/StringBuilder.h>
#include <wtf/text/StringToIntegerConversion.h>
#include <wtf/text/WTFString.h>

#if ENABLE(JOURNALD_LOG)
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

using namespace Inspector;

namespace JSC {

WTF_MAKE_TZONE_ALLOCATED_IMPL(ConsoleClient);

static void appendURLAndPosition(StringBuilder& builder, const String& url, unsigned lineNumber, unsigned columnNumber)
{
    if (url.isEmpty())
        return;

    builder.append(url);

    if (lineNumber > 0)
        builder.append(':', lineNumber);

    if (columnNumber > 0)
        builder.append(':', columnNumber);
}

static void appendMessagePrefix(StringBuilder& builder, MessageSource source, MessageType type, MessageLevel level)
{
    String sourceString;
    switch (source) {
    case MessageSource::ConsoleAPI:
        // Default, no need to be more specific.
        break;
    case MessageSource::XML:
        sourceString = "XML"_s;
        break;
    case MessageSource::JS:
        sourceString = "JS"_s;
        break;
    case MessageSource::Network:
        sourceString = "NETWORK"_s;
        break;
    case MessageSource::Storage:
        sourceString = "STORAGE"_s;
        break;
    case MessageSource::Rendering:
        sourceString = "RENDERING"_s;
        break;
    case MessageSource::CSS:
        sourceString = "CSS"_s;
        break;
    case MessageSource::Accessibility:
        sourceString = "Accessibility"_s;
        break;
    case MessageSource::Security:
        sourceString = "SECURITY"_s;
        break;
    case MessageSource::ContentBlocker:
        sourceString = "CONTENTBLOCKER"_s;
        break;
    case MessageSource::Media:
        sourceString = "MEDIA"_s;
        break;
    case MessageSource::MediaSource:
        sourceString = "MEDIASOURCE"_s;
        break;
    case MessageSource::WebRTC:
        sourceString = "WEBRTC"_s;
        break;
    case MessageSource::ITPDebug:
        sourceString = "ITPDEBUG"_s;
        break;
    case MessageSource::PrivateClickMeasurement:
        sourceString = "PRIVATECLICKMEASUREMENT"_s;
        break;
    case MessageSource::PaymentRequest:
        sourceString = "PAYMENTREQUEST"_s;
        break;
    case MessageSource::Other:
        sourceString = "OTHER"_s;
        break;
    }

    String typeString;
    switch (type) {
    case MessageType::Log:
        // Default, no need to be more specific.
        break;
    case MessageType::Clear:
        typeString = "CLEAR"_s;
        break;
    case MessageType::Dir:
        typeString = "DIR"_s;
        break;
    case MessageType::DirXML:
        typeString = "DIRXML"_s;
        break;
    case MessageType::Table:
        typeString = "TABLE"_s;
        break;
    case MessageType::Trace:
        typeString = "TRACE"_s;
        break;
    case MessageType::StartGroup:
        typeString = "STARTGROUP"_s;
        break;
    case MessageType::StartGroupCollapsed:
        typeString = "STARTGROUPCOLLAPSED"_s;
        break;
    case MessageType::EndGroup:
        typeString = "ENDGROUP"_s;
        break;
    case MessageType::Assert:
        typeString = "ASSERT"_s;
        break;
    case MessageType::Timing:
        typeString = "TIMING"_s;
        break;
    case MessageType::Profile:
        typeString = "PROFILE"_s;
        break;
    case MessageType::ProfileEnd:
        typeString = "PROFILEEND"_s;
        break;
    case MessageType::Image:
        typeString = "IMAGE"_s;
        break;
    }

    String levelString;
    switch (level) {
    case MessageLevel::Log:
        // Default, no need to be more specific.
        if (type == MessageType::Log)
            levelString = "LOG"_s;
        break;
    case MessageLevel::Debug:
        levelString = "DEBUG"_s;
        break;
    case MessageLevel::Info:
        levelString = "INFO"_s;
        break;
    case MessageLevel::Warning:
        levelString = "WARN"_s;
        break;
    case MessageLevel::Error:
        levelString = "ERROR"_s;
        break;
    }

    builder.append("CONSOLE"_s);
    if (!sourceString.isEmpty())
        builder.append(' ', sourceString);
    if (!typeString.isEmpty())
        builder.append(' ', typeString);
    if (!levelString.isEmpty())
        builder.append(' ', levelString);
}

#if ENABLE(JOURNALD_LOG)
static int journalPriority(MessageLevel level)
{
    switch (level) {
    case MessageLevel::Log:
    case MessageLevel::Info:
        return LOG_INFO;
    case MessageLevel::Debug:
        return LOG_DEBUG;
    case MessageLevel::Warning:
        return LOG_WARNING;
    case MessageLevel::Error:
        return LOG_ERR;
    }
    return LOG_INFO;
}

// Only use the native journal protocol when stderr is already connected to the journal, as systemd
// does for services: messages then end up in the same place, but multi-line ones are kept as a single entry.
static bool shouldLogConsoleMessagesToJournal()
{
    static const bool shouldLogToJournal = [] {
        if (!WTFShouldLogToJournal())
            return false;

        // systemd sets JOURNAL_STREAM to "<device>:<inode>" of the journal stream connected to stdout or stderr.
        auto journalStream = StringView::fromLatin1(getenv("JOURNAL_STREAM"));
        size_t separator = journalStream.find(':');
        if (separator == notFound)
            return false;

        auto device = parseInteger<uint64_t>(journalStream.left(separator));
        auto inode = parseInteger<uint64_t>(journalStream.substring(separator + 1));
        struct stat stderrStat;
        if (!device || !inode || fstat(STDERR_FILENO, &stderrStat))
            return false;

        return static_cast<uint64_t>(stderrStat.st_dev) == *device && static_cast<uint64_t>(stderrStat.st_ino) == *inode;
    }();

    return shouldLogToJournal;
}
#endif

static void logConsoleMessageToSystemConsole(MessageLevel level, const String& message, const String& url, unsigned lineNumber, unsigned columnNumber)
{
#if ENABLE(JOURNALD_LOG)
    if (shouldLogConsoleMessagesToJournal()) {
        SAFE_SD_JOURNAL_SEND("MESSAGE=%s", message.utf8(), "PRIORITY=%d", journalPriority(level),
            "WEBKIT_SUBSYSTEM=" LOG_CHANNEL_WEBKIT_SUBSYSTEM, "WEBKIT_CHANNEL=Console",
            "WEBKIT_CONSOLE_URL=%s", url.utf8(), "WEBKIT_CONSOLE_LINE=%u", lineNumber, "WEBKIT_CONSOLE_COLUMN=%u", columnNumber);
        return;
    }
#else
    UNUSED_PARAM(level);
    UNUSED_PARAM(url);
    UNUSED_PARAM(lineNumber);
    UNUSED_PARAM(columnNumber);
#endif
    SAFE_WTFLOGALWAYS("%s", message.utf8());
}

void ConsoleClient::printConsoleMessage(MessageSource source, MessageType type, MessageLevel level, const String& message, const String& url, unsigned lineNumber, unsigned columnNumber)
{
    StringBuilder builder;

    if (!url.isEmpty()) {
        appendURLAndPosition(builder, url, lineNumber, columnNumber);
        builder.append(": "_s);
    }

    appendMessagePrefix(builder, source, type, level);
    builder.append(' ', message);

    logConsoleMessageToSystemConsole(level, builder.toString(), url, lineNumber, columnNumber);
}

void ConsoleClient::printConsoleMessageWithArguments(MessageSource source, MessageType type, MessageLevel level, JSC::JSGlobalObject* globalObject, Ref<ScriptArguments>&& arguments)
{
    bool isTraceMessage = type == MessageType::Trace;
    size_t stackSize = isTraceMessage ? ScriptCallStack::maxCallStackSizeToCapture : 1;
    Ref<ScriptCallStack> callStack = createScriptCallStackForConsole(globalObject, stackSize);
    const ScriptCallFrame& lastCaller = callStack->at(0);

    StringBuilder builder(OverflowPolicy::RecordOverflow);

    if (!lastCaller.sourceURL().isEmpty()) {
        appendURLAndPosition(builder, lastCaller.sourceURL(), lastCaller.lineNumber(), lastCaller.columnNumber());
        builder.append(": "_s);
    }

    appendMessagePrefix(builder, source, type, level);
    for (size_t i = 0; i < arguments->argumentCount(); ++i) {
        builder.append(' ');
        auto* globalObject = arguments->globalObject();
        auto scope = DECLARE_TOP_EXCEPTION_SCOPE(globalObject->vm());
        builder.append(arguments->argumentAt(i).toWTFString(globalObject));
        scope.clearException();
    }

    if (isTraceMessage) {
        for (size_t i = 0; i < callStack->size(); ++i) {
            const ScriptCallFrame& callFrame = callStack->at(i);
            String functionName = callFrame.functionName();
            if (functionName.isEmpty())
                functionName = "(unknown)"_s;

            builder.append('\n', i, ": "_s, functionName, '(');
            appendURLAndPosition(builder, callFrame.sourceURL(), callFrame.lineNumber(), callFrame.columnNumber());
            builder.append(')');
        }
    }

    if (builder.hasOverflowed()) {
        WTFLogAlways("Console message exceeded maximum length.");
        return;
    }

    logConsoleMessageToSystemConsole(level, builder.toString(), lastCaller.sourceURL(), lastCaller.lineNumber(), lastCaller.columnNumber());
}

void ConsoleClient::internalMessageWithTypeAndLevel(MessageType type, MessageLevel level, JSC::JSGlobalObject* globalObject, Ref<ScriptArguments>&& arguments, ArgumentRequirement argumentRequirement)
{
    if (argumentRequirement == ArgumentRequirement::Yes && !arguments->argumentCount())
        return;

    messageWithTypeAndLevel(type, level, globalObject, WTF::move(arguments));
}

void ConsoleClient::logWithLevel(JSGlobalObject* globalObject, Ref<ScriptArguments>&& arguments, MessageLevel level)
{
    internalMessageWithTypeAndLevel(MessageType::Log, level, globalObject, WTF::move(arguments), ArgumentRequirement::Yes);
}

void ConsoleClient::clear(JSGlobalObject* globalObject)
{
    internalMessageWithTypeAndLevel(MessageType::Clear, MessageLevel::Log, globalObject, ScriptArguments::create(globalObject, { }), ArgumentRequirement::No);
}

void ConsoleClient::dir(JSGlobalObject* globalObject, Ref<ScriptArguments>&& arguments)
{
    internalMessageWithTypeAndLevel(MessageType::Dir, MessageLevel::Log, globalObject, WTF::move(arguments), ArgumentRequirement::Yes);
}

void ConsoleClient::dirXML(JSGlobalObject* globalObject, Ref<ScriptArguments>&& arguments)
{
    internalMessageWithTypeAndLevel(MessageType::DirXML, MessageLevel::Log, globalObject, WTF::move(arguments), ArgumentRequirement::Yes);
}

void ConsoleClient::table(JSGlobalObject* globalObject, Ref<ScriptArguments>&& arguments)
{
    internalMessageWithTypeAndLevel(MessageType::Table, MessageLevel::Log, globalObject, WTF::move(arguments), ArgumentRequirement::Yes);
}

void ConsoleClient::trace(JSGlobalObject* globalObject, Ref<ScriptArguments>&& arguments)
{
    internalMessageWithTypeAndLevel(MessageType::Trace, MessageLevel::Log, globalObject, WTF::move(arguments), ArgumentRequirement::No);
}

void ConsoleClient::assertion(JSGlobalObject* globalObject, Ref<ScriptArguments>&& arguments)
{
    internalMessageWithTypeAndLevel(MessageType::Assert, MessageLevel::Error, globalObject, WTF::move(arguments), ArgumentRequirement::No);
}

void ConsoleClient::group(JSGlobalObject* globalObject, Ref<ScriptArguments>&& arguments)
{
    internalMessageWithTypeAndLevel(MessageType::StartGroup, MessageLevel::Log, globalObject, WTF::move(arguments), ArgumentRequirement::No);
}

void ConsoleClient::groupCollapsed(JSGlobalObject* globalObject, Ref<ScriptArguments>&& arguments)
{
    internalMessageWithTypeAndLevel(MessageType::StartGroupCollapsed, MessageLevel::Log, globalObject, WTF::move(arguments), ArgumentRequirement::No);
}

void ConsoleClient::groupEnd(JSGlobalObject* globalObject, Ref<ScriptArguments>&& arguments)
{
    internalMessageWithTypeAndLevel(MessageType::EndGroup, MessageLevel::Log, globalObject, WTF::move(arguments), ArgumentRequirement::No);
}

} // namespace JSC
