#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE 1
#endif

#import <AVFoundation/AVCaptureDevice.h>
#import <AVFAudio/AVAudioEngine.h>
#import <AVFAudio/AVAudioFormat.h>
#import <AVFAudio/AVAudioNode.h>
#import <AVFAudio/AVSpeechSynthesis.h>
#import <Foundation/Foundation.h>
#import <Speech/Speech.h>

#include "native_voice_backend.h"

#include <dispatch/dispatch.h>
#include <pthread.h>
#include <stdio.h>
#include <stdatomic.h>
#include <string.h>
#include <time.h>

static const uint64_t kNativeVoiceTextStableMs = 700;
static const uint64_t kNativeVoiceAudioSilenceMs = 700;
static const NSTimeInterval kNativeVoiceSpeakerCooldownSeconds = 0.70;
static const NSUInteger kNativeVoiceMaxRecognitionFailures = 8;

static uint64_t monotonic_millis(void) {
    struct timespec now = {0};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

static BOOL buffer_has_speech_energy(AVAudioPCMBuffer *buffer) {
    AVAudioFrameCount frames = buffer.frameLength;
    AVAudioChannelCount channels = buffer.format.channelCount;
    if (frames == 0 || channels == 0)
        return NO;
    NSUInteger stride = buffer.format.isInterleaved ? channels : 1;
    double sum = 0.0;
    NSUInteger samples = 0;
    switch (buffer.format.commonFormat) {
        case AVAudioPCMFormatFloat32: {
            const float *data = buffer.floatChannelData ? buffer.floatChannelData[0] : NULL;
            if (!data) return NO;
            for (AVAudioFrameCount i = 0; i < frames; i += 2) {
                double sample = data[(NSUInteger)i * stride];
                sum += sample * sample;
                samples++;
            }
            break;
        }
        case AVAudioPCMFormatInt16: {
            const int16_t *data = buffer.int16ChannelData ? buffer.int16ChannelData[0] : NULL;
            if (!data) return NO;
            for (AVAudioFrameCount i = 0; i < frames; i += 2) {
                double sample = (double)data[(NSUInteger)i * stride] / 32768.0;
                sum += sample * sample;
                samples++;
            }
            break;
        }
        case AVAudioPCMFormatInt32: {
            const int32_t *data = buffer.int32ChannelData ? buffer.int32ChannelData[0] : NULL;
            if (!data) return NO;
            for (AVAudioFrameCount i = 0; i < frames; i += 2) {
                double sample = (double)data[(NSUInteger)i * stride] / 2147483648.0;
                sum += sample * sample;
                samples++;
            }
            break;
        }
        default:
            return NO;
    }
    /* About -38 dBFS: above ordinary room noise, below quiet near-field speech. */
    return samples > 0 && (sum / (double)samples) >= 0.00016;
}

@class DSCONativeVoiceOwner;

static pthread_mutex_t s_owner_mutex = PTHREAD_MUTEX_INITIALIZER;
static DSCONativeVoiceOwner *s_owner;

@interface DSCONativeVoiceRecognitionEvent : NSObject
@property(nonatomic, strong) SFSpeechRecognitionResult *result;
@property(nonatomic, strong) NSError *error;
@property(nonatomic) NSUInteger serial;
@end

@implementation DSCONativeVoiceRecognitionEvent
@end

@interface DSCONativeVoiceOwner : NSObject <AVSpeechSynthesizerDelegate> {
    atomic_uint_fast64_t _lastLoudAudioMs;
    atomic_bool _stopRequested;
    atomic_bool _finishedFlag;
    atomic_bool _tapInstalledFlag;
}
@property(nonatomic) native_voice_backend_callbacks_t callbacks;
@property(nonatomic) void *callbackContext;
@property(nonatomic) unsigned generation;
@property(atomic, strong) NSThread *thread;
@property(atomic, strong) NSRunLoop *runLoop;
@property(nonatomic, strong) NSPort *keepAlivePort;
@property(atomic, strong) AVAudioEngine *audioEngine;
@property(nonatomic, strong) SFSpeechRecognizer *recognizer;
@property(atomic, strong) NSOperationQueue *recognitionQueue;
@property(atomic, strong) SFSpeechAudioBufferRecognitionRequest *request;
@property(atomic, strong) SFSpeechRecognitionTask *task;
@property(atomic, strong) AVSpeechSynthesizer *synthesizer;
@property(nonatomic, strong) AVSpeechUtterance *currentUtterance;
@property(nonatomic, strong) NSTimer *silenceTimer;
@property(nonatomic, strong) NSTimer *restartTimer;
@property(nonatomic, strong) NSTimer *speechWatchdog;
@property(nonatomic, copy) NSString *lastPartial;
@property(nonatomic) uint64_t lastPartialAtMs;
@property(nonatomic) uint64_t speechStartedAtMs;
@property(nonatomic) uint64_t speechDeadlineMs;
@property(nonatomic) NSUInteger recognitionSerial;
@property(nonatomic) NSUInteger consecutiveFailures;
@property(atomic) BOOL stopping;
@property(nonatomic) BOOL speaking;
@property(atomic) BOOL finished;
@property(nonatomic) dispatch_group_t lifetimeGroup;

- (instancetype)initWithCallbacks:(const native_voice_backend_callbacks_t *)callbacks
                           context:(void *)context
                        generation:(unsigned)generation;
- (void)startThread;
- (void)requestStop;
- (void)beginSpeaking:(NSString *)text;
- (void)interruptSpeaking;
@end

static void owner_finished(DSCONativeVoiceOwner *owner) {
    pthread_mutex_lock(&s_owner_mutex);
    if (s_owner == owner)
        s_owner = nil;
    pthread_mutex_unlock(&s_owner_mutex);
}

@implementation DSCONativeVoiceOwner

- (instancetype)initWithCallbacks:(const native_voice_backend_callbacks_t *)callbacks
                           context:(void *)context
                        generation:(unsigned)generation {
    self = [super init];
    if (self) {
        _callbacks = *callbacks;
        _callbackContext = context;
        _generation = generation;
        _lastPartial = @"";
        atomic_init(&_lastLoudAudioMs, 0);
        atomic_init(&_stopRequested, false);
        atomic_init(&_finishedFlag, false);
        atomic_init(&_tapInstalledFlag, false);
        _lifetimeGroup = dispatch_group_create();
        dispatch_group_enter(_lifetimeGroup);
    }
    return self;
}

- (BOOL)stopping {
    return atomic_load_explicit(&_stopRequested, memory_order_acquire);
}

- (void)setStopping:(BOOL)stopping {
    atomic_store_explicit(&_stopRequested, stopping, memory_order_release);
}

- (BOOL)finished {
    return atomic_load_explicit(&_finishedFlag, memory_order_acquire);
}

- (void)setFinished:(BOOL)finished {
    atomic_store_explicit(&_finishedFlag, finished, memory_order_release);
}

- (void)emitState:(NSString *)phase detail:(NSString *)detail {
    if (self.callbacks.state)
        self.callbacks.state(self.callbackContext, self.generation,
                             phase.UTF8String ?: "", detail.UTF8String ?: "");
}

- (void)emitPartial:(NSString *)text {
    if (self.callbacks.partial)
        self.callbacks.partial(self.callbackContext, self.generation, text.UTF8String ?: "");
}

- (void)emitFinal:(NSString *)text {
    if (self.callbacks.final_text)
        self.callbacks.final_text(self.callbackContext, self.generation, text.UTF8String ?: "");
}

- (void)emitInactive:(NSString *)detail {
    if (self.callbacks.inactive)
        self.callbacks.inactive(self.callbackContext, self.generation,
                                "error", detail.UTF8String ?: "Native voice stopped.");
}

- (void)startThread {
    self.thread = [[NSThread alloc] initWithTarget:self selector:@selector(threadMain) object:nil];
    self.thread.name = @"dsco.native-voice";
    [self.thread start];
}

- (void)threadMain {
    @autoreleasepool {
        self.runLoop = [NSRunLoop currentRunLoop];
        self.keepAlivePort = [NSMachPort port];
        [self.runLoop addPort:self.keepAlivePort forMode:NSDefaultRunLoopMode];
        if (!self.stopping)
            [self beginAuthorization];
        while (!self.stopping) {
            @autoreleasepool {
                [self.runLoop runMode:NSDefaultRunLoopMode
                           beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.25]];
            }
        }
        [self shutdownResources];
        self.finished = YES;
        owner_finished(self);
        self.thread = nil;
        dispatch_group_leave(self.lifetimeGroup);
    }
}

- (void)performOnOwner:(SEL)selector object:(id)object {
    NSThread *thread = self.thread;
    if (!thread || self.finished)
        return;
    if ([NSThread currentThread] == thread) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Warc-performSelector-leaks"
        [self performSelector:selector withObject:object];
#pragma clang diagnostic pop
    } else
        [self performSelector:selector onThread:thread withObject:object waitUntilDone:NO];
}

- (void)beginAuthorization {
    if (self.stopping)
        return;
    /* Capability comes before TCC: do not ask for personal permissions on a
     * locale/device that cannot satisfy the on-device-only contract. */
    self.recognizer = [[SFSpeechRecognizer alloc] initWithLocale:[NSLocale currentLocale]];
    if (!self.recognizer) {
        [self failAndStop:@"Speech recognition is unavailable for the current locale."];
        return;
    }
    if (!self.recognizer.supportsOnDeviceRecognition) {
        [self failAndStop:@"On-device speech recognition is unavailable for the current locale. Cloud fallback is disabled."];
        return;
    }
    NSOperationQueue *recognitionQueue = [[NSOperationQueue alloc] init];
    recognitionQueue.name = @"dsco.native-voice.recognition";
    recognitionQueue.maxConcurrentOperationCount = 1;
    self.recognitionQueue = recognitionQueue;
    self.recognizer.queue = recognitionQueue;
    SFSpeechRecognizerAuthorizationStatus speech = [SFSpeechRecognizer authorizationStatus];
    if (speech == SFSpeechRecognizerAuthorizationStatusNotDetermined) {
        [self emitState:@"permission" detail:@"Waiting for Speech Recognition permission..."];
        __weak DSCONativeVoiceOwner *weakSelf = self;
        [SFSpeechRecognizer requestAuthorization:^(SFSpeechRecognizerAuthorizationStatus status) {
            DSCONativeVoiceOwner *owner = weakSelf;
            [owner performOnOwner:@selector(speechAuthorizationReturned:) object:@(status)];
        }];
        return;
    }
    [self speechAuthorizationReturned:@(speech)];
}

- (void)speechAuthorizationReturned:(NSNumber *)value {
    if (self.stopping)
        return;
    SFSpeechRecognizerAuthorizationStatus status =
        (SFSpeechRecognizerAuthorizationStatus)value.integerValue;
    if (status != SFSpeechRecognizerAuthorizationStatusAuthorized) {
        [self failAndStop:@"Speech Recognition permission denied. Enable it in System Settings > Privacy & Security > Speech Recognition."];
        return;
    }

    AVAuthorizationStatus mic = [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio];
    if (mic == AVAuthorizationStatusNotDetermined) {
        [self emitState:@"permission" detail:@"Waiting for microphone permission..."];
        __weak DSCONativeVoiceOwner *weakSelf = self;
        [AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio completionHandler:^(BOOL granted) {
            DSCONativeVoiceOwner *owner = weakSelf;
            [owner performOnOwner:@selector(microphoneAuthorizationReturned:)
                           object:@(granted)];
        }];
        return;
    }
    [self microphoneAuthorizationReturned:@(mic == AVAuthorizationStatusAuthorized)];
}

- (void)microphoneAuthorizationReturned:(NSNumber *)granted {
    if (self.stopping)
        return;
    if (!granted.boolValue) {
        [self failAndStop:@"Microphone permission denied. Enable it in System Settings > Privacy & Security > Microphone."];
        return;
    }
    [self startListening];
}

- (void)startListening {
    if (self.stopping || self.speaking)
        return;
    if (!self.recognizer.supportsOnDeviceRecognition) {
        [self failAndStop:@"On-device speech recognition became unavailable. Cloud fallback is disabled."];
        return;
    }

    [self stopRecognitionResources];
    self.recognitionSerial++;
    NSUInteger serial = self.recognitionSerial;
    self.lastPartial = @"";
    self.lastPartialAtMs = 0;
    atomic_store_explicit(&_lastLoudAudioMs, 0, memory_order_release);

    SFSpeechAudioBufferRecognitionRequest *request =
        [[SFSpeechAudioBufferRecognitionRequest alloc] init];
    request.shouldReportPartialResults = YES;
    request.requiresOnDeviceRecognition = YES;
    request.taskHint = SFSpeechRecognitionTaskHintDictation;
    self.request = request;

    __weak DSCONativeVoiceOwner *weakSelf = self;
    self.task = [self.recognizer recognitionTaskWithRequest:request
                                             resultHandler:^(SFSpeechRecognitionResult *result,
                                                             NSError *error) {
        DSCONativeVoiceOwner *owner = weakSelf;
        if (!owner)
            return;
        DSCONativeVoiceRecognitionEvent *event = [[DSCONativeVoiceRecognitionEvent alloc] init];
        event.result = result;
        event.error = error;
        event.serial = serial;
        [owner performOnOwner:@selector(handleRecognitionEvent:) object:event];
    }];

    AVAudioEngine *engine = [[AVAudioEngine alloc] init];
    self.audioEngine = engine;
    AVAudioInputNode *input = engine.inputNode;
    AVAudioFormat *format = [input outputFormatForBus:0];
    if (!input || format.sampleRate <= 0 || format.channelCount == 0) {
        [self failAndStop:@"No usable microphone input format is available."];
        return;
    }
    __weak SFSpeechAudioBufferRecognitionRequest *weakRequest = request;
    __weak DSCONativeVoiceOwner *weakAudioOwner = self;
    [input installTapOnBus:0 bufferSize:1024 format:format
                     block:^(AVAudioPCMBuffer *buffer, AVAudioTime *when) {
        (void)when;
        DSCONativeVoiceOwner *owner = weakAudioOwner;
        if (owner && buffer_has_speech_energy(buffer))
            atomic_store_explicit(&owner->_lastLoudAudioMs, monotonic_millis(),
                                  memory_order_release);
        [weakRequest appendAudioPCMBuffer:buffer];
    }];
    atomic_store_explicit(&_tapInstalledFlag, true, memory_order_release);
    [engine prepare];
    if (self.stopping) {
        [self stopRecognitionResources];
        return;
    }
    NSError *startError = nil;
    if (![engine startAndReturnError:&startError]) {
        NSString *message = startError.localizedDescription ?: @"unknown audio engine error";
        [self failAndStop:[NSString stringWithFormat:@"Unable to start the microphone: %@", message]];
        return;
    }
    if (self.stopping) {
        [self stopRecognitionResources];
        return;
    }

    self.silenceTimer = [NSTimer scheduledTimerWithTimeInterval:0.20
                                                        target:self
                                                      selector:@selector(checkSilence:)
                                                      userInfo:nil
                                                       repeats:YES];
    [self emitState:@"listening" detail:@"Listening on device..."];
}

- (void)handleRecognitionEvent:(DSCONativeVoiceRecognitionEvent *)event {
    if (self.stopping || self.speaking || event.serial != self.recognitionSerial)
        return;
    NSString *text = event.result.bestTranscription.formattedString ?: @"";
    if (text.length > 0 && ![text isEqualToString:self.lastPartial]) {
        self.consecutiveFailures = 0;
        self.lastPartial = text;
        self.lastPartialAtMs = monotonic_millis();
        [self emitPartial:text];
    }
    if (event.result.isFinal) {
        [self completePhrase:text];
        return;
    }
    if (event.error) {
        if (self.lastPartial.length > 0) {
            [self completePhrase:self.lastPartial];
        } else {
            self.consecutiveFailures++;
            self.recognitionSerial++;
            [self stopRecognitionResources];
            if (self.consecutiveFailures >= kNativeVoiceMaxRecognitionFailures) {
                NSString *reason = event.error.localizedDescription ?: @"unknown recognition error";
                [self failAndStop:[NSString stringWithFormat:
                    @"On-device speech recognition stopped after repeated errors: %@", reason]];
                return;
            }
            [self emitState:@"recovering" detail:@"Restarting on-device recognition..."];
            [self scheduleRestart:MIN(2.0, 0.25 * self.consecutiveFailures)];
        }
    }
}

- (void)checkSilence:(NSTimer *)timer {
    (void)timer;
    if (self.stopping || self.speaking || self.lastPartial.length == 0 ||
        self.lastPartialAtMs == 0)
        return;
    uint64_t now = monotonic_millis();
    uint64_t lastLoud = atomic_load_explicit(&_lastLoudAudioMs, memory_order_acquire);
    if (lastLoud > 0 && now >= self.lastPartialAtMs && now >= lastLoud &&
        now - self.lastPartialAtMs >= kNativeVoiceTextStableMs &&
        now - lastLoud >= kNativeVoiceAudioSilenceMs)
        [self completePhrase:self.lastPartial];
}

- (void)completePhrase:(NSString *)text {
    NSString *final = [text stringByTrimmingCharactersInSet:
                       [NSCharacterSet whitespaceAndNewlineCharacterSet]];
    self.recognitionSerial++; /* fence late result/error callbacks */
    [self stopRecognitionResources];
    self.lastPartial = @"";
    self.lastPartialAtMs = 0;
    [self emitPartial:@""];
    if (final.length > 0)
        [self emitFinal:final];
    if (!self.stopping && !self.speaking)
        [self scheduleRestart:0.08];
}

- (void)scheduleRestart:(NSTimeInterval)delay {
    [self.restartTimer invalidate];
    self.restartTimer = [NSTimer scheduledTimerWithTimeInterval:delay
                                                        target:self
                                                      selector:@selector(restartTimerFired:)
                                                      userInfo:nil
                                                       repeats:NO];
}

- (void)restartTimerFired:(NSTimer *)timer {
    if (timer != self.restartTimer)
        return;
    self.restartTimer = nil;
    if (self.speaking)
        self.speaking = NO;
    if (!self.stopping)
        [self startListening];
}

- (void)stopRecognitionResources {
    [self.silenceTimer invalidate];
    self.silenceTimer = nil;
    AVAudioEngine *engine = self.audioEngine;
    if (atomic_exchange_explicit(&_tapInstalledFlag, false, memory_order_acq_rel) && engine)
        [engine.inputNode removeTapOnBus:0];
    [engine stop];
    SFSpeechAudioBufferRecognitionRequest *request = self.request;
    SFSpeechRecognitionTask *task = self.task;
    [request endAudio];
    [task cancel];
    self.task = nil;
    self.request = nil;
    self.audioEngine = nil;
}

- (void)beginSpeaking:(NSString *)text {
    if (self.stopping || text.length == 0)
        return;
    [self.restartTimer invalidate];
    self.restartTimer = nil;
    self.recognitionSerial++;
    [self stopRecognitionResources];
    self.lastPartial = @"";
    [self emitPartial:@""];

    if (!self.synthesizer) {
        self.synthesizer = [[AVSpeechSynthesizer alloc] init];
        self.synthesizer.delegate = self;
    }
    if (self.synthesizer.isSpeaking) {
        [self.synthesizer stopSpeakingAtBoundary:AVSpeechBoundaryImmediate];
        /* A synchronous didCancel callback may have armed cooldown. The new
         * utterance supersedes it, so keep the microphone fenced. */
        [self.restartTimer invalidate];
        self.restartTimer = nil;
    }

    self.speaking = YES;
    [self emitState:@"speaking" detail:@"Speaking locally; microphone paused..."];
    AVSpeechUtterance *utterance = [[AVSpeechUtterance alloc] initWithString:text];
    NSString *language = [NSLocale preferredLanguages].firstObject;
    AVSpeechSynthesisVoice *voice = language ?
        [AVSpeechSynthesisVoice voiceWithLanguage:language] : nil;
    if (voice)
        utterance.voice = voice;
    self.currentUtterance = utterance;
    self.speechStartedAtMs = monotonic_millis();
    uint64_t estimatedMs = 5000u + (uint64_t)text.length * 85u;
    if (estimatedMs > 90000u) estimatedMs = 90000u;
    self.speechDeadlineMs = self.speechStartedAtMs + estimatedMs;
    [self.speechWatchdog invalidate];
    self.speechWatchdog = [NSTimer scheduledTimerWithTimeInterval:0.15
                                                          target:self
                                                        selector:@selector(checkSpeechWatchdog:)
                                                        userInfo:nil
                                                         repeats:YES];
    [self.synthesizer speakUtterance:utterance];
}

- (void)checkSpeechWatchdog:(NSTimer *)timer {
    (void)timer;
    if (self.stopping || !self.speaking || !self.currentUtterance)
        return;
    uint64_t now = monotonic_millis();
    BOOL failedToStartOrFinishedWithoutDelegate =
        now - self.speechStartedAtMs >= 400u && !self.synthesizer.isSpeaking;
    if (failedToStartOrFinishedWithoutDelegate || now >= self.speechDeadlineMs) {
        [self.synthesizer stopSpeakingAtBoundary:AVSpeechBoundaryImmediate];
        self.currentUtterance = nil;
        [self beginSpeakerCooldown];
    }
}

- (void)interruptSpeaking {
    if (self.stopping)
        return;
    if (self.synthesizer.isSpeaking)
        [self.synthesizer stopSpeakingAtBoundary:AVSpeechBoundaryImmediate];
    self.currentUtterance = nil;
    if (self.speaking)
        [self beginSpeakerCooldown];
}

- (void)speechSynthesizer:(AVSpeechSynthesizer *)synthesizer
 didFinishSpeechUtterance:(AVSpeechUtterance *)utterance {
    (void)synthesizer;
    [self performOnOwner:@selector(speechUtteranceEnded:) object:utterance];
}

- (void)speechSynthesizer:(AVSpeechSynthesizer *)synthesizer
 didCancelSpeechUtterance:(AVSpeechUtterance *)utterance {
    (void)synthesizer;
    [self performOnOwner:@selector(speechUtteranceEnded:) object:utterance];
}

- (void)speechUtteranceEnded:(AVSpeechUtterance *)utterance {
    if (self.stopping || utterance != self.currentUtterance)
        return;
    self.currentUtterance = nil;
    [self beginSpeakerCooldown];
}

- (void)beginSpeakerCooldown {
    if (self.stopping)
        return;
    [self.speechWatchdog invalidate];
    self.speechWatchdog = nil;
    self.speaking = YES; /* echo fence stays raised throughout cooldown */
    [self emitState:@"cooldown" detail:@"Waiting for speaker echo to clear..."];
    [self scheduleRestart:kNativeVoiceSpeakerCooldownSeconds];
}

- (void)failAndStop:(NSString *)detail {
    if (self.stopping)
        return;
    [self emitInactive:detail];
    [self stopOnOwnerThread];
}

- (void)shutdownResources {
    [self.restartTimer invalidate];
    self.restartTimer = nil;
    [self.speechWatchdog invalidate];
    self.speechWatchdog = nil;
    self.recognitionSerial++;
    [self stopRecognitionResources];
    [self.recognitionQueue cancelAllOperations];
    self.recognitionQueue = nil;
    self.recognizer = nil;
    self.synthesizer.delegate = nil;
    [self.synthesizer stopSpeakingAtBoundary:AVSpeechBoundaryImmediate];
    self.currentUtterance = nil;
    self.synthesizer = nil;
    self.speaking = NO;
}

- (void)stopOnOwnerThread {
    self.stopping = YES;
    [self shutdownResources];
    if (self.runLoop)
        CFRunLoopStop([self.runLoop getCFRunLoop]);
}

- (void)stopRequest:(id)unused {
    (void)unused;
    [self stopOnOwnerThread];
}

- (void)interruptRequest:(id)unused {
    (void)unused;
    [self interruptSpeaking];
}

- (void)requestStop {
    if (self.finished)
        return;
    self.stopping = YES;
    if (!self.thread) {
        /* startThread may be racing backend_stop after publication in
         * s_owner. Record the stop before the owner run loop exists. */
        return;
    }
    if ([NSThread currentThread] == self.thread) {
        [self stopOnOwnerThread];
        return;
    }
    [self performOnOwner:@selector(stopRequest:) object:nil];
}

- (void)forceCloseAudioAfterTimeout {
    /* Last-resort cleanup after the owner run loop missed its bounded stop
     * deadline. AVAudioEngine stop/cancel are thread-safe control operations;
     * no PCM data is retained or inspected here. */
    self.stopping = YES;
    AVAudioEngine *engine = self.audioEngine;
    [engine stop];
    SFSpeechAudioBufferRecognitionRequest *request = self.request;
    SFSpeechRecognitionTask *task = self.task;
    AVSpeechSynthesizer *synthesizer = self.synthesizer;
    [request endAudio];
    [task cancel];
    [synthesizer stopSpeakingAtBoundary:AVSpeechBoundaryImmediate];
    NSRunLoop *runLoop = self.runLoop;
    if (runLoop)
        CFRunLoopStop([runLoop getCFRunLoop]);
}

@end

bool native_voice_backend_start(const native_voice_backend_callbacks_t *callbacks,
                                void *context, unsigned generation,
                                char *error, size_t error_cap) {
    if (!callbacks || !callbacks->inactive || !callbacks->final_text) {
        if (error && error_cap)
            snprintf(error, error_cap, "Native voice callbacks are incomplete.");
        return false;
    }
    if (@available(macOS 10.15, *)) {
        pthread_mutex_lock(&s_owner_mutex);
        DSCONativeVoiceOwner *existing = s_owner;
        if (existing && !existing.finished) {
            pthread_mutex_unlock(&s_owner_mutex);
            [existing requestStop];
            if (error && error_cap)
                snprintf(error, error_cap,
                         "A previous native voice session is still stopping; try voice on again.");
            return false;
        }
        DSCONativeVoiceOwner *owner =
            [[DSCONativeVoiceOwner alloc] initWithCallbacks:callbacks
                                                   context:context
                                                generation:generation];
        s_owner = owner;
        pthread_mutex_unlock(&s_owner_mutex);
        [owner startThread];
        return true;
    }
    if (error && error_cap)
        snprintf(error, error_cap, "Native on-device voice requires macOS 10.15 or later.");
    return false;
}

void native_voice_backend_stop(unsigned generation) {
    pthread_mutex_lock(&s_owner_mutex);
    DSCONativeVoiceOwner *owner =
        (s_owner && s_owner.generation == generation) ? s_owner : nil;
    pthread_mutex_unlock(&s_owner_mutex);
    if (!owner)
        return;
    BOOL ownerCallback = [NSThread currentThread] == owner.thread;
    [owner requestStop];
    if (!ownerCallback) {
        /* Cleanup is synchronous from the C caller's perspective, but cannot
         * wedge terminal shutdown forever if an OS framework misbehaves. */
        long waitResult = dispatch_group_wait(
            owner.lifetimeGroup, dispatch_time(DISPATCH_TIME_NOW, 1500 * NSEC_PER_MSEC));
        if (waitResult != 0) {
            [owner forceCloseAudioAfterTimeout];
            (void)dispatch_group_wait(
                owner.lifetimeGroup, dispatch_time(DISPATCH_TIME_NOW, 250 * NSEC_PER_MSEC));
        }
    }
}

void native_voice_backend_speak(const char *text, unsigned generation) {
    if (!text || !text[0])
        return;
    pthread_mutex_lock(&s_owner_mutex);
    DSCONativeVoiceOwner *owner =
        (s_owner && s_owner.generation == generation) ? s_owner : nil;
    pthread_mutex_unlock(&s_owner_mutex);
    if (!owner)
        return;
    NSString *speech = [NSString stringWithUTF8String:text];
    if (!speech.length)
        return;
    [owner performOnOwner:@selector(beginSpeaking:) object:speech];
}

void native_voice_backend_interrupt(unsigned generation) {
    pthread_mutex_lock(&s_owner_mutex);
    DSCONativeVoiceOwner *owner =
        (s_owner && s_owner.generation == generation) ? s_owner : nil;
    pthread_mutex_unlock(&s_owner_mutex);
    [owner performOnOwner:@selector(interruptRequest:) object:nil];
}
