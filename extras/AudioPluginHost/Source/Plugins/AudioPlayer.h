/*
  ==============================================================================

   This file is part of the JUCE library.
   Copyright (c) 2022 - Raw Material Software Limited

   JUCE is an open source library subject to commercial or open-source
   licensing.

   By using JUCE, you agree to the terms of both the JUCE 7 End-User License
   Agreement and JUCE Privacy Policy.

   End User License Agreement: www.juce.com/juce-7-licence
   Privacy Policy: www.juce.com/juce-privacy-policy

   Or: You may also use this code under the terms of the GPL v3 (see
   www.gnu.org/licenses).

   JUCE IS PROVIDED "AS IS" WITHOUT ANY WARRANTY, AND ALL WARRANTIES, WHETHER
   EXPRESSED OR IMPLIED, INCLUDING MERCHANTABILITY AND FITNESS FOR PURPOSE, ARE
   DISCLAIMED.

  ==============================================================================
*/

/*
    Internal continuous audio source for AudioPluginHost.
    Reuses the Sampler demo's reader factories, Sample and waveform UI.
*/
#pragma once

#include "../../../../examples/Plugins/SamplerPluginDemo.h"
#include <cmath>
#include <limits>

class AudioPlayerProcessor final : public AudioProcessor, private Timer, private ChangeBroadcaster
{
public:
    AudioPlayerProcessor()
        : AudioProcessor (BusesProperties().withOutput ("Output", AudioChannelSet::stereo(), true))
    {
        startTimer (100);
    }

    ~AudioPlayerProcessor() override
    {
        stopTimer();
        delete pending.exchange (nullptr);
        delete retired.exchange (nullptr);
    }

    const String getName() const override                 { return "Audio Player"; }
    bool acceptsMidi() const override                     { return true; }
    bool producesMidi() const override                    { return false; }
    bool isMidiEffect() const override                    { return false; }
    bool supportsDoublePrecisionProcessing() const override { return true; }
    double getTailLengthSeconds() const override          { return 0.0; }
    bool hasEditor() const override                       { return true; }
    AudioProcessorEditor* createEditor() override;
    int getNumPrograms() override                         { return 1; }
    int getCurrentProgram() override                      { return 0; }
    void setCurrentProgram (int) override                 {}
    const String getProgramName (int) override            { return "None"; }
    void changeProgramName (int, const String&) override  {}
    // AudioPluginHost saves/restores internal plugin state on the message thread.
    // Persist the file reference, not the decoded audio or active MIDI toggle.
    void getStateInformation (MemoryBlock& destination) override
    {
        XmlElement state ("AudioPlayerState");
        state.setAttribute ("version", 1);
        state.setAttribute ("filePath", selectedFilePath);
        copyXmlToBinary (state, destination);
    }

    void setStateInformation (const void* data, int size) override
    {
        if (size == 0) // Graphs saved before file persistence was added.
        {
            AudioFormatManager manager;
            setSample (nullptr, manager);
            return;
        }

        auto state = getXmlFromBinary (data, size);
        if (state == nullptr || ! state->hasTagName ("AudioPlayerState")
            || state->getIntAttribute ("version") != 1)
            return;

        const auto path = state->getStringAttribute ("filePath");
        AudioFormatManager manager;
        manager.registerBasicFormats();
        if (path.isEmpty())
            setSample (nullptr, manager);
        else if (! File::isAbsolutePath (path) || ! setFile (File (path), manager))
        {
            // Never keep unrelated old audio after a failed restore. Retain the
            // reference so a later graph save does not discard the missing path.
            setSample (nullptr, manager);
            selectedFilePath = path;
            fileUnavailable = true;
            sendChangeMessage();
        }
    }

    // Message thread only, used by both the editor and graph restoration.
    bool setFile (const File& file, AudioFormatManager& manager)
    {
        if (! setSample (std::make_unique<FileAudioFormatReaderFactory> (file), manager))
            return false;

        selectedFilePath = file.getFullPathName();
        sendChangeMessage();
        return true;
    }

    bool isBusesLayoutSupported (const BusesLayout& layouts) const override
    {
        return layouts.getMainInputChannelSet().isDisabled()
            && (layouts.getMainOutputChannelSet() == AudioChannelSet::mono()
             || layouts.getMainOutputChannelSet() == AudioChannelSet::stereo());
    }

    void prepareToPlay (double rate, int) override
    {
        outputSampleRate = rate;
        reset();
    }

    void reset() override
    {
        activeNote = -1;
        position = 0.0;
        playbackPosition.store (0.0f, std::memory_order_relaxed);
    }

    void releaseResources() override { reset(); }
    void processBlock (AudioBuffer<float>& b, MidiBuffer& m) override  { process (b, m); }
    void processBlock (AudioBuffer<double>& b, MidiBuffer& m) override { process (b, m); }

    // Message thread only. Decode the whole file before publishing it to the audio thread.
    bool setSample (std::unique_ptr<AudioFormatReaderFactory> factory, AudioFormatManager& manager)
    {
        std::unique_ptr<Sample> loaded;
        if (factory != nullptr)
        {
            auto reader = factory->make (manager);
            if (reader == nullptr || ! std::isfinite (reader->sampleRate) || reader->sampleRate <= 0.0
                || reader->numChannels == 0 || reader->lengthInSamples <= 0
                || reader->lengthInSamples > std::numeric_limits<int>::max() - 4)
                return false;

            try
            {
                // Sample normally caps files at 10 seconds. Include every frame here,
                // with half a frame of rounding headroom for its integer conversion.
                loaded = std::make_unique<Sample> (*reader,
                    (static_cast<double> (reader->lengthInSamples) + 0.5) / reader->sampleRate);
            }
            catch (const std::exception&)
            {
                return false;
            }
        }

        // Exchange ownership without locking. A superseded pending load and the
        // retired file are destroyed here on the message thread.
        timerCallback();
        delete pending.exchange (new LoadedFile { std::move (loaded) });
        readerFactory = std::move (factory); // GUI-owned snapshot, also used when reopening the editor.
        selectedFilePath.clear();
        fileUnavailable = false;
        sendChangeMessage();
        return true;
    }

private:
    template <typename Element>
    void render (AudioBuffer<Element>& buffer, int start, int end)
    {
        const auto* sample = current != nullptr ? current->sample.get() : nullptr;
        if (activeNote < 0 || sample == nullptr || outputSampleRate <= 0.0)
            return;

        const auto& data = sample->getBuffer();
        const auto length = sample->getLength();
        const auto* left = data.getReadPointer (0);
        const auto* right = data.getNumChannels() > 1 ? data.getReadPointer (1) : left;
        const auto increment = sample->getSampleRate() / outputSampleRate;

        for (auto i = start; i < end; ++i)
        {
            const auto index = static_cast<int> (position);
            const auto next = index + 1 == length ? 0 : index + 1;
            const auto fraction = static_cast<float> (position - index);
            const auto l = left[index] + fraction * (left[next] - left[index]);
            const auto r = right[index] + fraction * (right[next] - right[index]);

            if (buffer.getNumChannels() == 1)
                buffer.setSample (0, i, static_cast<Element> ((l + r) * 0.5f));
            else if (buffer.getNumChannels() >= 2)
            {
                buffer.setSample (0, i, static_cast<Element> (l));
                buffer.setSample (1, i, static_cast<Element> (r));
            }

            position += increment;
            if (position >= length)
                position = std::fmod (position, static_cast<double> (length));
        }
    }

    template <typename Element>
    void process (AudioBuffer<Element>& buffer, MidiBuffer& midi)
    {
        ScopedNoDenormals noDenormals;
        // If the GUI has not reclaimed the previous file yet, defer adoption.
        // This keeps retirement bounded and never frees audio data in the callback.
        if (retired.load() == nullptr)
            if (auto* next = pending.exchange (nullptr))
            {
                auto* previous = current.release();
                current.reset (next);
                retired.store (previous);
                reset();
            }
        auto* sample = current != nullptr ? current->sample.get() : nullptr;
        buffer.clear();
        int rendered = 0;
        for (const auto metadata : midi)
        {
            const auto eventPosition = jlimit (rendered, buffer.getNumSamples(), metadata.samplePosition);
            render (buffer, rendered, eventPosition);
            rendered = eventPosition;
            if (metadata.numBytes > 3)
                continue; // Avoid allocating a MidiMessage for SysEx in the audio callback.
            const auto message = metadata.getMessage();
            if (message.isNoteOn())
            {
                const auto note = message.getNoteNumber();
                if (activeNote == note)
                    reset();
                else if (activeNote < 0 && sample != nullptr)
                {
                    activeNote = note;
                    position = 0.0;
                }
                // Other note numbers are ignored while playing. Channels and velocity
                // do not affect playback; note-offs, sustain and pitch bend are ignored.
            }
        }
        render (buffer, rendered, buffer.getNumSamples());
        playbackPosition.store (sample != nullptr ? static_cast<float> (position / sample->getSampleRate()) : 0.0f,
                                std::memory_order_relaxed);
        midi.clear();
    }

    class Editor final : public AudioProcessorEditor, public FileDragAndDropTarget, private ChangeListener
    {
    public:
        explicit Editor (AudioPlayerProcessor& p)
            : AudioProcessorEditor (&p), owner (p), dataModel (formatManager),
              waveform (dataModel, [&p] { return std::vector<float> { p.playbackPosition.load (std::memory_order_relaxed) }; },
                        undoManager)
        {
            formatManager.registerBasicFormats();
            fileChooser = std::make_unique<FileChooser> ("Select a file to load...", File(),
                                                         formatManager.getWildcardForAllFormats());
            addAndMakeVisible (waveform);
            addAndMakeVisible (loadButton);
            addAndMakeVisible (instructions);
            addAndMakeVisible (fileLabel);
            instructions.setText ("Note-on: start / same note: stop. Other notes ignored. Full file loops at original speed.",
                                  dontSendNotification);
            loadButton.onClick = [this]
            {
                fileChooser->launchAsync (FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles,
                                         [this] (const FileChooser& chooser)
                {
                    if (chooser.getResult() != File())
                        load (chooser.getResult());
                });
            };
            owner.addChangeListener (this);
            changeListenerCallback (&owner);
            setResizable (true, true);
            setResizeLimits (640, 480, 2560, 1440);
            setSize (640, 480);
        }

        ~Editor() override { owner.removeChangeListener (this); }

    private:
        void changeListenerCallback (ChangeBroadcaster*) override
        {
            dataModel.setSampleReader (owner.readerFactory != nullptr ? owner.readerFactory->clone() : nullptr, nullptr);
            const auto text = owner.selectedFilePath.isEmpty() ? String ("No audio file loaded")
                : (owner.fileUnavailable ? String ("Unable to load: ") : String()) + owner.selectedFilePath;
            fileLabel.setText (text, dontSendNotification);
            fileLabel.setTooltip (text);
        }

        void resized() override
        {
            auto bounds = getLocalBounds();
            auto top = bounds.removeFromTop (50);
            loadButton.setBounds (top.removeFromRight (100).reduced (4));
            instructions.setBounds (top.reduced (4));
            fileLabel.setBounds (bounds.removeFromBottom (25).reduced (4, 0));
            waveform.setBounds (bounds);
        }

        bool isInterestedInFileDrag (const StringArray& files) override
        {
            WildcardFileFilter filter (formatManager.getWildcardForAllFormats(), {}, "Known Audio Formats");
            return files.size() == 1 && filter.isFileSuitable (files[0]);
        }

        void filesDropped (const StringArray& files, int, int) override
        {
            if (files.size() == 1)
                load (files[0]);
        }

        void load (const File& file)
        {
            if (! owner.setFile (file, formatManager))
                AlertWindow::showMessageBoxAsync (MessageBoxIconType::WarningIcon, "Audio Player",
                                                  "Unable to load the complete audio file. Check its format, length and available memory.");
        }

        AudioPlayerProcessor& owner;
        AudioFormatManager formatManager;
        DataModel dataModel;
        UndoManager undoManager;
        WaveformEditor waveform;
        TextButton loadButton { "Load New Sample" };
        Label instructions, fileLabel;
        std::unique_ptr<FileChooser> fileChooser;
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Editor)
    };

    struct LoadedFile { std::unique_ptr<Sample> sample; };
    void timerCallback() override { delete retired.exchange (nullptr); }

    // SPSC ownership: GUI publishes pending/reclaims retired; audio owns current.
    // All exchanges use sequential consistency; no shared_ptr destruction or locks.
    std::atomic<LoadedFile*> pending { nullptr }, retired { nullptr };
    std::unique_ptr<LoadedFile> current;
    std::unique_ptr<AudioFormatReaderFactory> readerFactory;
    String selectedFilePath; // Message-thread state; never accessed by the audio callback.
    bool fileUnavailable = false;
    double outputSampleRate = 44100.0;
    double position = 0.0;
    int activeNote = -1; // Sole playback state: -1 stopped, otherwise the starting note number.
    std::atomic<float> playbackPosition { 0.0f };
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioPlayerProcessor)
};

inline AudioProcessorEditor* AudioPlayerProcessor::createEditor() { return new Editor (*this); }
