#include "../identity.h"
#include "../jade_assert.h"
#include "../keychain.h"
#include "../multisig.h"
#include "../ui.h"
#include "../utils/cbor_rpc.h"

#include "process_utils.h"

// Identity, curve and index are always needed by the 'identity' functions.
bool params_identity_curve_index(CborValue* params, const char** identity, size_t* identity_len, const char** curve,
    size_t* curve_len, size_t* index, const char** errmsg)
{
    JADE_ASSERT(params);
    JADE_INIT_OUT_PPTR(identity);
    JADE_INIT_OUT_SIZE(identity_len);
    JADE_INIT_OUT_PPTR(curve);
    JADE_INIT_OUT_SIZE(curve_len);
    JADE_INIT_OUT_SIZE(index);
    JADE_INIT_OUT_PPTR(errmsg);

    rpc_get_string_ptr("identity", params, identity, identity_len);
    if (!*identity || *identity_len >= MAX_DISPLAY_MESSAGE_LEN
        || !is_identity_protocol_valid(*identity, *identity_len)) {
        *errmsg = "Failed to extract valid identity from parameters";
        return false;
    }

    rpc_get_string_ptr("curve", params, curve, curve_len);
    if (!*curve || !is_identity_curve_valid(*curve, *curve_len)) {
        *errmsg = "Failed to extract valid curve name from parameters";
        return false;
    }

    // index is optional
    if (rpc_has_field_data("index", params)) {
        if (!rpc_get_sizet("index", params, index)) {
            *errmsg = "Failed to extract valid index from parameters";
            return false;
        }
    }

    return true;
}

// Hash-prevouts and output index are needed to generate deterministic blinding factors.
bool params_hashprevouts_outputindex(CborValue* params, const uint8_t** hash_prevouts, size_t* hash_prevouts_len,
    size_t* output_index, const char** errmsg)
{
    JADE_ASSERT(params);
    JADE_INIT_OUT_PPTR(hash_prevouts);
    JADE_INIT_OUT_SIZE(hash_prevouts_len);
    JADE_INIT_OUT_SIZE(output_index);
    JADE_INIT_OUT_PPTR(errmsg);

    rpc_get_bytes_ptr("hash_prevouts", params, hash_prevouts, hash_prevouts_len);
    if (*hash_prevouts_len != SHA256_LEN) {
        *errmsg = "Failed to extract hash_prevouts from parameters";
        return false;
    }

    if (!rpc_get_sizet("output_index", params, output_index)) {
        *errmsg = "Failed to extract output index from parameters";
        return false;
    }

    return true;
}

// Read multisig name and load the registration record.
bool params_load_multisig(CborValue* params, char* multisig_name, const size_t multisig_name_len,
    multisig_data_t* multisig_data, const char** errmsg)
{
    JADE_ASSERT(params);
    JADE_ASSERT(multisig_name);
    JADE_ASSERT(multisig_name_len);
    JADE_ASSERT(multisig_data);
    JADE_INIT_OUT_PPTR(errmsg);

    size_t written = 0;
    rpc_get_string("multisig_name", multisig_name_len, params, multisig_name, &written);
    if (written == 0) {
        *errmsg = "Invalid multisig name parameter";
        return false;
    }

    if (!multisig_load_from_storage(multisig_name, multisig_data, errmsg)) {
        // 'errmsg' populated by above call
        return false;
    }

    return true;
}

// Take a multisig record as above, then read out the signer path suffixes and derive the relevant pubkeys.
// Output any warning messages associated with the signer paths (eg. if they are non-standard, mismatch, etc)
// Required for generating multisig receive addresses and also change addresses (when auto-validating change).
bool params_multisig_pubkeys(const bool is_change, CborValue* params, multisig_data_t* multisig_data, uint8_t* pubkeys,
    const size_t pubkeys_len, size_t* pubkeys_written, char* warningmsg, size_t warningmsg_len, const char** errmsg)
{
    JADE_ASSERT(params);
    JADE_ASSERT(multisig_data);
    JADE_ASSERT(pubkeys);
    JADE_ASSERT(pubkeys_len == MAX_MULTISIG_SIGNERS * EC_PUBLIC_KEY_LEN);
    JADE_INIT_OUT_SIZE(pubkeys_written);
    JADE_ASSERT(warningmsg);
    JADE_ASSERT(warningmsg_len);
    JADE_INIT_OUT_PPTR(errmsg);

    // Validate paths
    CborValue all_signer_paths;
    bool all_paths_as_expected;
    bool final_elements_consistent;
    if (!rpc_get_array("paths", params, &all_signer_paths)
        || !multisig_validate_paths(is_change, &all_signer_paths, &all_paths_as_expected, &final_elements_consistent)) {
        *errmsg = "Failed to extract signer paths from parameters";
        return false;
    }

    // If paths not as expected show a warning message and ask the user to confirm
    if (!all_paths_as_expected || !final_elements_consistent) {
        const char* msg1
            = !all_paths_as_expected ? (is_change ? "Unusual multisig change path." : "Unusual multisig path.") : "";
        const char* msg2
            = !final_elements_consistent ? "Non-standard multisig with different paths across signers." : "";
        const char* maybe_space = !all_paths_as_expected && !final_elements_consistent ? " " : "";

        const int ret = snprintf(
            warningmsg, warningmsg_len, "Warning: %s%s%s Proceed at your own risk.", msg1, maybe_space, msg2);
        JADE_ASSERT(ret > 0 && ret < warningmsg_len);
    }

    if (!multisig_get_pubkeys(
            multisig_data->xpubs, multisig_data->num_xpubs, &all_signer_paths, pubkeys, pubkeys_len, pubkeys_written)
        || *pubkeys_written != multisig_data->num_xpubs * EC_PUBLIC_KEY_LEN) {
        *errmsg = "Unexpected number of signer paths or invalid path for multisig";
        return false;
    }

    return true;
}

// Get the relevant master blinding key (padded to 64-bytes for low-level calls).
// This may be the master key with a multisig registration (if indicated in the parameters).
// Otherwise, defaults to the master blinding key directly associated with this wallet/signer.
bool params_get_master_blindingkey(
    CborValue* params, uint8_t* master_blinding_key, const size_t master_blinding_key_len, const char** errmsg)
{
    JADE_ASSERT(params);
    JADE_ASSERT(master_blinding_key);
    JADE_ASSERT(master_blinding_key_len == HMAC_SHA512_LEN);
    JADE_ASSERT(errmsg);

    // If no 'multisig_name' parameter, default to the signer's own master blinding key
    if (!rpc_has_field_data("multisig_name", params)) {
        memcpy(master_blinding_key, keychain_get()->master_unblinding_key, master_blinding_key_len);
        return true;
    }

    // If is multisig, extract master key from multisig record
    size_t written = 0;
    char multisig_name[MAX_MULTISIG_NAME_SIZE];
    rpc_get_string("multisig_name", sizeof(multisig_name), params, multisig_name, &written);
    if (written == 0) {
        *errmsg = "Invalid multisig name parameter";
        return false;
    }

    multisig_data_t multisig_data = { 0 };
    if (!multisig_load_from_storage(multisig_name, &multisig_data, errmsg)) {
        // 'errmsg' populated by above call
        return false;
    }

    if (!multisig_get_master_blinding_key(&multisig_data, master_blinding_key, master_blinding_key_len, errmsg)) {
        // 'errmsg' populated by above call
        return false;
    }

    return true;
}