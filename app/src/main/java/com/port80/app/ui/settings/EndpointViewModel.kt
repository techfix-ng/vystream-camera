package com.port80.app.ui.settings

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.port80.app.data.EndpointProfileRepository
import com.port80.app.data.model.EndpointProfile
import com.port80.app.data.qr.QrEndpointImportParser
import com.port80.app.data.qr.QrEndpointParseResult
import com.port80.app.service.ActiveStreamStateProvider
import com.port80.app.discovery.DiscoveredObs
import com.port80.app.discovery.ObsDiscoveryClient
import dagger.hilt.android.lifecycle.HiltViewModel
import kotlinx.coroutines.flow.*
import kotlinx.coroutines.launch
import java.util.UUID
import javax.inject.Inject

/**
 * ViewModel for managing RTMP endpoint profiles.
 * Handles CRUD operations for saved streaming destinations.
 */
@HiltViewModel
class EndpointViewModel @Inject constructor(
    private val profileRepository: EndpointProfileRepository,
    private val activeStreamStateProvider: ActiveStreamStateProvider,
    private val obsDiscoveryClient: ObsDiscoveryClient
) : ViewModel() {

    /** All saved profiles, observed by the UI. */
    val profiles: StateFlow<List<EndpointProfile>> = profileRepository.getAll()
        .stateIn(viewModelScope, SharingStarted.Eagerly, emptyList())

    /** Currently selected profile for editing. */
    private val _editingProfile = MutableStateFlow<EndpointProfile?>(null)
    val editingProfile: StateFlow<EndpointProfile?> = _editingProfile.asStateFlow()

    /** True when StreamingService owns the camera and QR scanning must wait. */
    val isStreamActive: StateFlow<Boolean> = activeStreamStateProvider.isStreamActive
        .stateIn(viewModelScope, SharingStarted.WhileSubscribed(5000), false)

    /** Dialog state for QR-import decisions that need a user answer. */
    private val _importDialogState = MutableStateFlow<EndpointImportDialogState>(EndpointImportDialogState.None)
    val importDialogState: StateFlow<EndpointImportDialogState> = _importDialogState.asStateFlow()

    /** Holds a parsed profile while the UI asks duplicate/default questions. */
    private var pendingImportedProfile: EndpointProfile? = null

    private val _discoveredObs = MutableStateFlow<List<DiscoveredObs>>(emptyList())
    val discoveredObs: StateFlow<List<DiscoveredObs>> = _discoveredObs.asStateFlow()

    private val _isDiscovering = MutableStateFlow(false)
    val isDiscovering: StateFlow<Boolean> = _isDiscovering.asStateFlow()

    /** Most recent reachable OBS/plugin endpoint, used to prefill manual entry. */
    private val _suggestedObs = MutableStateFlow<DiscoveredObs?>(null)

    init {
        // Warm the suggestion so Manual Entry is normally populated immediately.
        refreshSuggestedObs()
    }

    fun discoverObs() {
        if (_isDiscovering.value) return
        viewModelScope.launch {
            _isDiscovering.value = true
            val devices = runCatching { obsDiscoveryClient.discover() }.getOrDefault(emptyList())
                .filter(::isReachableEndpoint)
            _discoveredObs.value = devices
            _suggestedObs.value = devices.firstOrNull() ?: _suggestedObs.value
            _isDiscovering.value = false
        }
    }

    private fun refreshSuggestedObs() {
        viewModelScope.launch {
            val discovered = runCatching { obsDiscoveryClient.discover() }.getOrDefault(emptyList())
                .firstOrNull(::isReachableEndpoint)
            if (discovered != null) _suggestedObs.value = discovered
        }
    }

    private fun isReachableEndpoint(device: DiscoveredObs): Boolean =
        device.address.isNotBlank() &&
            device.address != "0.0.0.0" &&
            device.address != "::" &&
            device.address != "127.0.0.1" &&
            device.srtPort in 1..65535

    private fun manualProfile(device: DiscoveredObs? = _suggestedObs.value) = EndpointProfile(
        id = UUID.randomUUID().toString(),
        name = device?.assignedCameraName.orEmpty(),
        url = device?.let { "srt://${it.address}:${it.srtPort}" }.orEmpty(),
        streamKey = "",
        srtLatencyMs = 200,
        srtMode = com.port80.app.data.model.SrtMode.CALLER,
        srtStreamId = device?.pairingToken
    )

    fun addDiscoveredObs(device: DiscoveredObs) {
        val profile = EndpointProfile(
            id = UUID.randomUUID().toString(),
            name = device.assignedCameraName,
            url = "srt://${device.address}:${device.srtPort}",
            srtLatencyMs = 200,
            srtMode = com.port80.app.data.model.SrtMode.CALLER,
            srtStreamId = device.pairingToken
        )
        // A newly paired desktop becomes the active destination immediately.
        // Existing profiles remain saved and can be selected again at any time.
        saveProfile(profile.copy(isDefault = true))
        _discoveredObs.value = emptyList()
    }

    fun dismissDiscovery() {
        _discoveredObs.value = emptyList()
        _isDiscovering.value = false
    }

    fun selectProfile(profile: EndpointProfile) {
        _editingProfile.value = profile
    }

    fun dismissEdit() {
        _editingProfile.value = null
    }

    fun newProfile() {
        _editingProfile.value = manualProfile()

        // Keep the editor usable while discovery finishes, and never overwrite
        // fields after the user has started typing.
        if (_suggestedObs.value == null) {
            viewModelScope.launch {
                val discovered = runCatching { obsDiscoveryClient.discover() }.getOrDefault(emptyList())
                    .firstOrNull(::isReachableEndpoint)
                    ?: return@launch
                _suggestedObs.value = discovered
                val current = _editingProfile.value ?: return@launch
                if (current.name.isBlank() && current.url.isBlank()) {
                    _editingProfile.value = current.copy(
                        name = discovered.assignedCameraName,
                        url = "srt://${discovered.address}:${discovered.srtPort}",
                        srtLatencyMs = 200,
                        srtMode = com.port80.app.data.model.SrtMode.CALLER,
                        srtStreamId = discovered.pairingToken
                    )
                }
            }
        }
    }

    fun saveProfile(profile: EndpointProfile) {
        viewModelScope.launch {
            profileRepository.save(profile)
            if (profile.isDefault) {
                // The repository stores the default profile as a separate ID.
                // Saving the profile alone is not enough to mark it default.
                profileRepository.setDefault(profile.id)
            }
            _editingProfile.value = null
        }
    }

    /** Parse a raw QR result and move it into confirmation/editing UI. */
    fun onQrScanned(rawText: String) {
        if (activeStreamStateProvider.isStreamActive.value) {
            _importDialogState.value = EndpointImportDialogState.BlockedStreamActive
            return
        }

        when (val result = QrEndpointImportParser.parse(rawText)) {
            is QrEndpointParseResult.Invalid -> {
                _importDialogState.value = EndpointImportDialogState.InvalidPayload(result.reason)
            }
            is QrEndpointParseResult.Success -> {
                prepareImportedProfile(
                    result.candidate.toProfile(
                        id = UUID.randomUUID().toString(),
                        isDefault = result.candidate.requestedDefault
                    )
                )
            }
        }
    }

    /** Clear whichever QR-import dialog is currently visible. */
    fun dismissImportDialog() {
        _importDialogState.value = EndpointImportDialogState.None
        pendingImportedProfile = null
    }

    /** User approved updating an existing endpoint with scanned values. */
    fun confirmDuplicateUpdate() {
        val profile = pendingImportedProfile ?: return
        _importDialogState.value = EndpointImportDialogState.None
        if (profile.isDefault) {
            _importDialogState.value = EndpointImportDialogState.ConfirmDefault
        } else {
            _editingProfile.value = profile
            pendingImportedProfile = null
        }
    }

    /** User answered whether the imported profile should become the default. */
    fun confirmDefaultImport(applyAsDefault: Boolean) {
        val profile = pendingImportedProfile ?: return
        _importDialogState.value = EndpointImportDialogState.None
        pendingImportedProfile = null
        _editingProfile.value = profile.copy(isDefault = applyAsDefault)
    }

    private fun prepareImportedProfile(importedProfile: EndpointProfile) {
        val duplicate = findDuplicate(importedProfile)
        val profileForEditor = if (duplicate != null) {
            // Preserve identity/default status from the saved endpoint. The QR
            // code can update editable fields only after the user confirms.
            importedProfile.copy(id = duplicate.id, isDefault = duplicate.isDefault || importedProfile.isDefault)
        } else {
            importedProfile
        }

        pendingImportedProfile = profileForEditor
        _importDialogState.value = when {
            duplicate != null -> EndpointImportDialogState.ConfirmDuplicateUpdate(
                existingName = duplicate.name.ifBlank { "Unnamed profile" }
            )
            profileForEditor.isDefault -> EndpointImportDialogState.ConfirmDefault
            else -> {
                pendingImportedProfile = null
                _editingProfile.value = profileForEditor
                EndpointImportDialogState.None
            }
        }
    }

    private fun findDuplicate(importedProfile: EndpointProfile): EndpointProfile? {
        val importedKey = QrEndpointImportParser.duplicateKey(importedProfile)
        return profiles.value.firstOrNull { saved ->
            QrEndpointImportParser.duplicateKey(saved) == importedKey
        }
    }

    fun deleteProfile(id: String) {
        viewModelScope.launch {
            profileRepository.delete(id)
            if (_editingProfile.value?.id == id) {
                _editingProfile.value = null
            }
        }
    }

    fun setDefault(id: String) {
        viewModelScope.launch {
            profileRepository.setDefault(id)
        }
    }
}

/** Dialogs the endpoint screen may show while importing a scanned endpoint. */
sealed interface EndpointImportDialogState {
    data object None : EndpointImportDialogState
    data object BlockedStreamActive : EndpointImportDialogState
    data object ConfirmDefault : EndpointImportDialogState
    data class InvalidPayload(val reason: String) : EndpointImportDialogState
    data class ConfirmDuplicateUpdate(val existingName: String) : EndpointImportDialogState
}
