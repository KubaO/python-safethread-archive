# Generated from 'Sound.h'

def FOUR_CHAR_CODE(x): return x
soundListRsrc = FOUR_CHAR_CODE('snd ')
kSimpleBeepID = 1
# rate48khz = (long)0xBB800000
# rate44khz = (long)0xAC440000
rate32khz = 0x7D000000
rate22050hz = 0x56220000
rate22khz = 0x56EE8BA3
rate16khz = 0x3E800000
rate11khz = 0x2B7745D1
rate11025hz = 0x2B110000
rate8khz = 0x1F400000
sampledSynth = 5
squareWaveSynth = 1
waveTableSynth = 3
MACE3snthID = 11
MACE6snthID = 13
kMiddleC = 60
kNoVolume = 0
kFullVolume = 0x0100
stdQLength = 128
dataOffsetFlag = 0x8000
kUseOptionalOutputDevice = -1
notCompressed = 0
fixedCompression = -1
variableCompression = -2
twoToOne = 1
eightToThree = 2
threeToOne = 3
sixToOne = 4
sixToOnePacketSize = 8
threeToOnePacketSize = 16
stateBlockSize = 64
leftOverBlockSize = 32
firstSoundFormat = 0x0001
secondSoundFormat = 0x0002
dbBufferReady = 0x00000001
dbLastBuffer = 0x00000004
sysBeepDisable = 0x0000
sysBeepEnable = (1 << 0)
sysBeepSynchronous = (1 << 1)
unitTypeNoSelection = 0xFFFF
unitTypeSeconds = 0x0000
stdSH = 0x00
extSH = 0xFF
cmpSH = 0xFE
nullCmd = 0
quietCmd = 3
flushCmd = 4
reInitCmd = 5
waitCmd = 10
pauseCmd = 11
resumeCmd = 12
callBackCmd = 13
syncCmd = 14
availableCmd = 24
versionCmd = 25
volumeCmd = 46
getVolumeCmd = 47
clockComponentCmd = 50
getClockComponentCmd = 51
scheduledSoundCmd = 52
linkSoundComponentsCmd = 53
soundCmd = 80
bufferCmd = 81
rateMultiplierCmd = 86
getRateMultiplierCmd = 87
initCmd = 1
freeCmd = 2
totalLoadCmd = 26
loadCmd = 27
freqDurationCmd = 40
restCmd = 41
freqCmd = 42
ampCmd = 43
timbreCmd = 44
getAmpCmd = 45
waveTableCmd = 60
phaseCmd = 61
rateCmd = 82
continueCmd = 83
doubleBufferCmd = 84
getRateCmd = 85
sizeCmd = 90
convertCmd = 91
waveInitChannelMask = 0x07
waveInitChannel0 = 0x04
waveInitChannel1 = 0x05
waveInitChannel2 = 0x06
waveInitChannel3 = 0x07
initChan0 = waveInitChannel0
initChan1 = waveInitChannel1
initChan2 = waveInitChannel2
initChan3 = waveInitChannel3
outsideCmpSH = 0
insideCmpSH = 1
aceSuccess = 0
aceMemFull = 1
aceNilBlock = 2
aceBadComp = 3
aceBadEncode = 4
aceBadDest = 5
aceBadCmd = 6
initChanLeft = 0x0002
initChanRight = 0x0003
initNoInterp = 0x0004
initNoDrop = 0x0008
initMono = 0x0080
initStereo = 0x00C0
initMACE3 = 0x0300
initMACE6 = 0x0400
initPanMask = 0x0003
initSRateMask = 0x0030
initStereoMask = 0x00C0
initCompMask = 0xFF00
siActiveChannels = FOUR_CHAR_CODE('chac')
siActiveLevels = FOUR_CHAR_CODE('lmac')
siAGCOnOff = FOUR_CHAR_CODE('agc ')
siAsync = FOUR_CHAR_CODE('asyn')
siAVDisplayBehavior = FOUR_CHAR_CODE('avdb')
siChannelAvailable = FOUR_CHAR_CODE('chav')
siCompressionAvailable = FOUR_CHAR_CODE('cmav')
siCompressionChannels = FOUR_CHAR_CODE('cpct')
siCompressionFactor = FOUR_CHAR_CODE('cmfa')
siCompressionHeader = FOUR_CHAR_CODE('cmhd')
siCompressionNames = FOUR_CHAR_CODE('cnam')
siCompressionParams = FOUR_CHAR_CODE('evaw')
siCompressionSampleRate = FOUR_CHAR_CODE('cprt')
siCompressionType = FOUR_CHAR_CODE('comp')
siContinuous = FOUR_CHAR_CODE('cont')
siDecompressionParams = FOUR_CHAR_CODE('wave')
siDeviceBufferInfo = FOUR_CHAR_CODE('dbin')
siDeviceConnected = FOUR_CHAR_CODE('dcon')
siDeviceIcon = FOUR_CHAR_CODE('icon')
siDeviceName = FOUR_CHAR_CODE('name')
siEQSpectrumBands = FOUR_CHAR_CODE('eqsb')
siEQSpectrumLevels = FOUR_CHAR_CODE('eqlv')
siEQSpectrumOnOff = FOUR_CHAR_CODE('eqlo')
siEQSpectrumResolution = FOUR_CHAR_CODE('eqrs')
siEQToneControlGain = FOUR_CHAR_CODE('eqtg')
siEQToneControlOnOff = FOUR_CHAR_CODE('eqtc')
siHardwareBalance = FOUR_CHAR_CODE('hbal')
siHardwareBalanceSteps = FOUR_CHAR_CODE('hbls')
siHardwareBass = FOUR_CHAR_CODE('hbas')
siHardwareBassSteps = FOUR_CHAR_CODE('hbst')
siHardwareBusy = FOUR_CHAR_CODE('hwbs')
siHardwareFormat = FOUR_CHAR_CODE('hwfm')
siHardwareMute = FOUR_CHAR_CODE('hmut')
siHardwareMuteNoPrefs = FOUR_CHAR_CODE('hmnp')
siHardwareTreble = FOUR_CHAR_CODE('htrb')
siHardwareTrebleSteps = FOUR_CHAR_CODE('hwts')
siHardwareVolume = FOUR_CHAR_CODE('hvol')
siHardwareVolumeSteps = FOUR_CHAR_CODE('hstp')
siHeadphoneMute = FOUR_CHAR_CODE('pmut')
siHeadphoneVolume = FOUR_CHAR_CODE('pvol')
siHeadphoneVolumeSteps = FOUR_CHAR_CODE('hdst')
siInputAvailable = FOUR_CHAR_CODE('inav')
siInputGain = FOUR_CHAR_CODE('gain')
siInputSource = FOUR_CHAR_CODE('sour')
siInputSourceNames = FOUR_CHAR_CODE('snam')
siLevelMeterOnOff = FOUR_CHAR_CODE('lmet')
siModemGain = FOUR_CHAR_CODE('mgai')
siMonitorAvailable = FOUR_CHAR_CODE('mnav')
siMonitorSource = FOUR_CHAR_CODE('mons')
siNumberChannels = FOUR_CHAR_CODE('chan')
siOptionsDialog = FOUR_CHAR_CODE('optd')
siOSTypeInputSource = FOUR_CHAR_CODE('inpt')
siOSTypeInputAvailable = FOUR_CHAR_CODE('inav')
siOutputDeviceName = FOUR_CHAR_CODE('onam')
siPlayThruOnOff = FOUR_CHAR_CODE('plth')
siPostMixerSoundComponent = FOUR_CHAR_CODE('psmx')
siPreMixerSoundComponent = FOUR_CHAR_CODE('prmx')
siQuality = FOUR_CHAR_CODE('qual')
siRateMultiplier = FOUR_CHAR_CODE('rmul')
siRecordingQuality = FOUR_CHAR_CODE('qual')
siSampleRate = FOUR_CHAR_CODE('srat')
siSampleRateAvailable = FOUR_CHAR_CODE('srav')
siSampleSize = FOUR_CHAR_CODE('ssiz')
siSampleSizeAvailable = FOUR_CHAR_CODE('ssav')
siSetupCDAudio = FOUR_CHAR_CODE('sucd')
siSetupModemAudio = FOUR_CHAR_CODE('sumd')
siSlopeAndIntercept = FOUR_CHAR_CODE('flap')
siSoundClock = FOUR_CHAR_CODE('sclk')
siUseThisSoundClock = FOUR_CHAR_CODE('sclc')
siSpeakerMute = FOUR_CHAR_CODE('smut')
siSpeakerVolume = FOUR_CHAR_CODE('svol')
siSSpCPULoadLimit = FOUR_CHAR_CODE('3dll')
siSSpLocalization = FOUR_CHAR_CODE('3dif')
siSSpSpeakerSetup = FOUR_CHAR_CODE('3dst')
siStereoInputGain = FOUR_CHAR_CODE('sgai')
siSubwooferMute = FOUR_CHAR_CODE('bmut')
siTerminalType = FOUR_CHAR_CODE('ttyp')
siTwosComplementOnOff = FOUR_CHAR_CODE('twos')
siVendorProduct = FOUR_CHAR_CODE('vpro')
siVolume = FOUR_CHAR_CODE('volu')
siVoxRecordInfo = FOUR_CHAR_CODE('voxr')
siVoxStopInfo = FOUR_CHAR_CODE('voxs')
siWideStereo = FOUR_CHAR_CODE('wide')
siSupportedExtendedFlags = FOUR_CHAR_CODE('exfl')
siRateConverterRollOffSlope = FOUR_CHAR_CODE('rcdb')
siOutputLatency = FOUR_CHAR_CODE('olte')
siCloseDriver = FOUR_CHAR_CODE('clos')
siInitializeDriver = FOUR_CHAR_CODE('init')
siPauseRecording = FOUR_CHAR_CODE('paus')
siUserInterruptProc = FOUR_CHAR_CODE('user')
# kInvalidSource = (long)0xFFFFFFFF
kNoSource = FOUR_CHAR_CODE('none')
kCDSource = FOUR_CHAR_CODE('cd  ')
kExtMicSource = FOUR_CHAR_CODE('emic')
kSoundInSource = FOUR_CHAR_CODE('sinj')
kRCAInSource = FOUR_CHAR_CODE('irca')
kTVFMTunerSource = FOUR_CHAR_CODE('tvfm')
kDAVInSource = FOUR_CHAR_CODE('idav')
kIntMicSource = FOUR_CHAR_CODE('imic')
kMediaBaySource = FOUR_CHAR_CODE('mbay')
kModemSource = FOUR_CHAR_CODE('modm')
kPCCardSource = FOUR_CHAR_CODE('pcm ')
kZoomVideoSource = FOUR_CHAR_CODE('zvpc')
kDVDSource = FOUR_CHAR_CODE('dvda')
kMicrophoneArray = FOUR_CHAR_CODE('mica')
kNoSoundComponentType = FOUR_CHAR_CODE('****')
kSoundComponentType = FOUR_CHAR_CODE('sift')
kSoundComponentPPCType = FOUR_CHAR_CODE('nift')
kRate8SubType = FOUR_CHAR_CODE('ratb')
kRate16SubType = FOUR_CHAR_CODE('ratw')
kConverterSubType = FOUR_CHAR_CODE('conv')
kSndSourceSubType = FOUR_CHAR_CODE('sour')
kMixerType = FOUR_CHAR_CODE('mixr')
kMixer8SubType = FOUR_CHAR_CODE('mixb')
kMixer16SubType = FOUR_CHAR_CODE('mixw')
kSoundInputDeviceType = FOUR_CHAR_CODE('sinp')
kWaveInSubType = FOUR_CHAR_CODE('wavi')
kWaveInSnifferSubType = FOUR_CHAR_CODE('wisn')
kSoundOutputDeviceType = FOUR_CHAR_CODE('sdev')
kClassicSubType = FOUR_CHAR_CODE('clas')
kASCSubType = FOUR_CHAR_CODE('asc ')
kDSPSubType = FOUR_CHAR_CODE('dsp ')
kAwacsSubType = FOUR_CHAR_CODE('awac')
kGCAwacsSubType = FOUR_CHAR_CODE('awgc')
kSingerSubType = FOUR_CHAR_CODE('sing')
kSinger2SubType = FOUR_CHAR_CODE('sng2')
kWhitSubType = FOUR_CHAR_CODE('whit')
kSoundBlasterSubType = FOUR_CHAR_CODE('sbls')
kWaveOutSubType = FOUR_CHAR_CODE('wavo')
kWaveOutSnifferSubType = FOUR_CHAR_CODE('wosn')
kDirectSoundSubType = FOUR_CHAR_CODE('dsnd')
kDirectSoundSnifferSubType = FOUR_CHAR_CODE('dssn')
kUNIXsdevSubType = FOUR_CHAR_CODE('un1x')
kUSBSubType = FOUR_CHAR_CODE('usb ')
kBlueBoxSubType = FOUR_CHAR_CODE('bsnd')
kSoundCompressor = FOUR_CHAR_CODE('scom')
kSoundDecompressor = FOUR_CHAR_CODE('sdec')
kAudioComponentType = FOUR_CHAR_CODE('adio')
kAwacsPhoneSubType = FOUR_CHAR_CODE('hphn')
kAudioVisionSpeakerSubType = FOUR_CHAR_CODE('telc')
kAudioVisionHeadphoneSubType = FOUR_CHAR_CODE('telh')
kPhilipsFaderSubType = FOUR_CHAR_CODE('tvav')
kSGSToneSubType = FOUR_CHAR_CODE('sgs0')
kSoundEffectsType = FOUR_CHAR_CODE('snfx')
kEqualizerSubType = FOUR_CHAR_CODE('eqal')
kSSpLocalizationSubType = FOUR_CHAR_CODE('snd3')
kSoundNotCompressed = FOUR_CHAR_CODE('NONE')
k8BitOffsetBinaryFormat = FOUR_CHAR_CODE('raw ')
k16BitBigEndianFormat = FOUR_CHAR_CODE('twos')
k16BitLittleEndianFormat = FOUR_CHAR_CODE('sowt')
kFloat32Format = FOUR_CHAR_CODE('fl32')
kFloat64Format = FOUR_CHAR_CODE('fl64')
k24BitFormat = FOUR_CHAR_CODE('in24')
k32BitFormat = FOUR_CHAR_CODE('in32')
k32BitLittleEndianFormat = FOUR_CHAR_CODE('23ni')
kMACE3Compression = FOUR_CHAR_CODE('MAC3')
kMACE6Compression = FOUR_CHAR_CODE('MAC6')
kCDXA4Compression = FOUR_CHAR_CODE('cdx4')
kCDXA2Compression = FOUR_CHAR_CODE('cdx2')
kIMACompression = FOUR_CHAR_CODE('ima4')
kULawCompression = FOUR_CHAR_CODE('ulaw')
kALawCompression = FOUR_CHAR_CODE('alaw')
kMicrosoftADPCMFormat = 0x6D730002
kDVIIntelIMAFormat = 0x6D730011
kDVAudioFormat = FOUR_CHAR_CODE('dvca')
kQDesignCompression = FOUR_CHAR_CODE('QDMC')
kQDesign2Compression = FOUR_CHAR_CODE('QDM2')
kQUALCOMMCompression = FOUR_CHAR_CODE('Qclp')
kOffsetBinary = k8BitOffsetBinaryFormat
kTwosComplement = k16BitBigEndianFormat
kLittleEndianFormat = k16BitLittleEndianFormat
kMPEGLayer3Format = 0x6D730055
kFullMPEGLay3Format = FOUR_CHAR_CODE('.mp3')
k16BitNativeEndianFormat = k16BitLittleEndianFormat
k16BitNonNativeEndianFormat = k16BitBigEndianFormat
k16BitNativeEndianFormat = k16BitBigEndianFormat
k16BitNonNativeEndianFormat = k16BitLittleEndianFormat
k8BitRawIn = (1 << 0)
k8BitTwosIn = (1 << 1)
k16BitIn = (1 << 2)
kStereoIn = (1 << 3)
k8BitRawOut = (1 << 8)
k8BitTwosOut = (1 << 9)
k16BitOut = (1 << 10)
kStereoOut = (1 << 11)
kReverse = (1 << 16)
kRateConvert = (1 << 17)
kCreateSoundSource = (1 << 18)
kVMAwareness = (1 << 21)
kHighQuality = (1 << 22)
kNonRealTime = (1 << 23)
kSourcePaused = (1 << 0)
kPassThrough = (1 << 16)
kNoSoundComponentChain = (1 << 17)
kNoMixing = (1 << 0)
kNoSampleRateConversion = (1 << 1)
kNoSampleSizeConversion = (1 << 2)
kNoSampleFormatConversion = (1 << 3)
kNoChannelConversion = (1 << 4)
kNoDecompression = (1 << 5)
kNoVolumeConversion = (1 << 6)
kNoRealtimeProcessing = (1 << 7)
kScheduledSource = (1 << 8)
kNonInterleavedBuffer = (1 << 9)
kNonPagingMixer = (1 << 10)
kSoundConverterMixer = (1 << 11)
kPagingMixer = (1 << 12)
kVMAwareMixer = (1 << 13)
kExtendedSoundData = (1 << 14)
kBestQuality = (1 << 0)
kInputMask = 0x000000FF
kOutputMask = 0x0000FF00
kOutputShift = 8
kActionMask = 0x00FF0000
kSoundComponentBits = 0x00FFFFFF
kAudioFormatAtomType = FOUR_CHAR_CODE('frma')
kAudioEndianAtomType = FOUR_CHAR_CODE('enda')
kAudioVBRAtomType = FOUR_CHAR_CODE('vbra')
kAudioTerminatorAtomType = 0
kAVDisplayHeadphoneRemove = 0
kAVDisplayHeadphoneInsert = 1
kAVDisplayPlainTalkRemove = 2
kAVDisplayPlainTalkInsert = 3
audioAllChannels = 0
audioLeftChannel = 1
audioRightChannel = 2
audioUnmuted = 0
audioMuted = 1
audioDoesMono = (1 << 0)
audioDoesStereo = (1 << 1)
audioDoesIndependentChannels = (1 << 2)
siCDQuality = FOUR_CHAR_CODE('cd  ')
siBestQuality = FOUR_CHAR_CODE('best')
siBetterQuality = FOUR_CHAR_CODE('betr')
siGoodQuality = FOUR_CHAR_CODE('good')
siNoneQuality = FOUR_CHAR_CODE('none')
siDeviceIsConnected = 1
siDeviceNotConnected = 0
siDontKnowIfConnected = -1
siReadPermission = 0
siWritePermission = 1
kSoundConverterDidntFillBuffer = (1 << 0)
kSoundConverterHasLeftOverData = (1 << 1)
kExtendedSoundSampleCountNotValid = 1 << 0
kExtendedSoundBufferSizeValid = 1 << 1
kScheduledSoundDoScheduled = 1 << 0
kScheduledSoundDoCallBack = 1 << 1
kScheduledSoundExtendedHdr = 1 << 2
kSoundComponentInitOutputDeviceSelect = 0x0001
kSoundComponentSetSourceSelect = 0x0002
kSoundComponentGetSourceSelect = 0x0003
kSoundComponentGetSourceDataSelect = 0x0004
kSoundComponentSetOutputSelect = 0x0005
kSoundComponentAddSourceSelect = 0x0101
kSoundComponentRemoveSourceSelect = 0x0102
kSoundComponentGetInfoSelect = 0x0103
kSoundComponentSetInfoSelect = 0x0104
kSoundComponentStartSourceSelect = 0x0105
kSoundComponentStopSourceSelect = 0x0106
kSoundComponentPauseSourceSelect = 0x0107
kSoundComponentPlaySourceBufferSelect = 0x0108
kAudioGetVolumeSelect = 0x0000
kAudioSetVolumeSelect = 0x0001
kAudioGetMuteSelect = 0x0002
kAudioSetMuteSelect = 0x0003
kAudioSetToDefaultsSelect = 0x0004
kAudioGetInfoSelect = 0x0005
kAudioGetBassSelect = 0x0006
kAudioSetBassSelect = 0x0007
kAudioGetTrebleSelect = 0x0008
kAudioSetTrebleSelect = 0x0009
kAudioGetOutputDeviceSelect = 0x000A
kAudioMuteOnEventSelect = 0x0081
kDelegatedSoundComponentSelectors = 0x0100
kSndInputReadAsyncSelect = 0x0001
kSndInputReadSyncSelect = 0x0002
kSndInputPauseRecordingSelect = 0x0003
kSndInputResumeRecordingSelect = 0x0004
kSndInputStopRecordingSelect = 0x0005
kSndInputGetStatusSelect = 0x0006
kSndInputGetDeviceInfoSelect = 0x0007
kSndInputSetDeviceInfoSelect = 0x0008
kSndInputInitHardwareSelect = 0x0009
