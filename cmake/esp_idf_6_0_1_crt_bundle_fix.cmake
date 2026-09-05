# SPDX-License-Identifier: Apache-2.0
#
# Compatibility backport for ESP-IDF v6.0.1 issue IDFGH-17627. The upstream
# esp_crt_bundle fix references issuer/subject buffers whose owners outlive the
# temporary candidate CA instead of allocating buffers that mbedTLS does not
# release. Remove this shim after moving to an ESP-IDF release containing the
# upstream fix.

if(NOT CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_CROSS_SIGNED_VERIFY)
    return()
endif()

set(_crt_bundle_source
    "$ENV{IDF_PATH}/components/mbedtls/esp_crt_bundle/esp_crt_bundle.c")
set(_crt_bundle_v6_0_1_sha256
    "e44d1e0a42a9d33cfc072ea005e93c8a0337c5ebcbfb9a1cf1554930f4f2816f")

if(NOT EXISTS "${_crt_bundle_source}")
    message(FATAL_ERROR
        "ESP-IDF certificate-bundle source not found: ${_crt_bundle_source}")
endif()

file(SHA256 "${_crt_bundle_source}" _crt_bundle_actual_sha256)
if(NOT _crt_bundle_actual_sha256 STREQUAL _crt_bundle_v6_0_1_sha256)
    message(FATAL_ERROR
        "Unsupported esp_crt_bundle.c (${_crt_bundle_actual_sha256}). "
        "This compatibility backport is restricted to the audited ESP-IDF "
        "v6.0.1 source (${_crt_bundle_v6_0_1_sha256}); remove or re-audit it "
        "before changing ESP-IDF versions.")
endif()

file(READ "${_crt_bundle_source}" _crt_bundle_contents)
string(REPLACE "\r\n" "\n" _crt_bundle_contents "${_crt_bundle_contents}")

set(_old_asn1_helper [=[static int esp_crt_copy_asn1(const mbedtls_asn1_named_data *src, mbedtls_asn1_named_data *dst)
{
    if (src == NULL || dst == NULL) {
        return -1;
    }

    dst->oid.tag = src->oid.tag;
    dst->oid.len = src->oid.len;
    dst->oid.p = calloc(1, src->oid.len);
    if (dst->oid.p == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory for OID");
        return -1;
    }
    memcpy(dst->oid.p, src->oid.p, src->oid.len);
    dst->val.tag = src->val.tag;
    dst->val.len = src->val.len;
    dst->val.p = calloc(1, src->val.len);
    if (dst->val.p == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory for value");
        free(dst->oid.p);
        return -1;
    }
    memcpy(dst->val.p, src->val.p, src->val.len);
    return 0;
}]=])

set(_new_asn1_helper [=[/* Reference ASN.1 named data by pointing into src's buffers.
 * The src data must outlive dst. */
static int esp_crt_ref_asn1(const mbedtls_asn1_named_data *src,
                            mbedtls_asn1_named_data *dst)
{
    if (src == NULL || dst == NULL) {
        return -1;
    }

    dst->oid.tag = src->oid.tag;
    dst->oid.len = src->oid.len;
    dst->oid.p = src->oid.p;
    dst->val.tag = src->val.tag;
    dst->val.len = src->val.len;
    dst->val.p = src->val.p;
    dst->next_merged = src->next_merged;
    return 0;
}]=])

set(_old_subject_raw [=[    new_cert->subject_raw.tag = MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE;
    new_cert->subject_raw.len = cert_name_len;
    new_cert->subject_raw.p = calloc(1, cert_name_len);
    if (new_cert->subject_raw.p == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory for subject");
        mbedtls_x509_crt_free(new_cert);
        free(new_cert);
        return MBEDTLS_ERR_X509_ALLOC_FAILED;
    }
    memcpy(new_cert->subject_raw.p, cert_name, cert_name_len);]=])

set(_new_subject_raw [=[    /* The certificate bundle remains alive for the handshake. */
    new_cert->subject_raw.tag = MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE;
    new_cert->subject_raw.len = cert_name_len;
    new_cert->subject_raw.p = (unsigned char *)cert_name;]=])

set(_old_issuer_copy [=[    // Loop through the child->issuer and copy the values to the new certificate
    const mbedtls_asn1_named_data *child_issuer = &child->issuer;
    mbedtls_asn1_named_data *parent_subject = &new_cert->subject;
    while (child_issuer != NULL) {
        if (esp_crt_copy_asn1(child_issuer, parent_subject) != 0) {
            ESP_LOGE(TAG, "Failed to copy ASN.1 data");
            mbedtls_x509_crt_free(new_cert);
            free(new_cert);
            return MBEDTLS_ERR_X509_ALLOC_FAILED;
        }
        child_issuer = child_issuer->next;
        if (child_issuer == NULL) {
            break;
        }

        if (parent_subject->next == NULL) {
            parent_subject->next = calloc(1, sizeof(mbedtls_asn1_named_data));
            if (parent_subject->next == NULL) {
                ESP_LOGE(TAG, "Failed to allocate memory for next issuer");
                mbedtls_x509_crt_free(new_cert);
                free(new_cert);
                return MBEDTLS_ERR_X509_ALLOC_FAILED;
            }
            parent_subject = parent_subject->next;
        }
    }]=])

set(_new_issuer_copy [=[    /* Populate parent subject by referencing the child issuer. The child
     * certificate owns the referenced buffers for the whole verification. */
    const mbedtls_asn1_named_data *child_issuer = &child->issuer;
    mbedtls_asn1_named_data *parent_subject = &new_cert->subject;
    if (esp_crt_ref_asn1(child_issuer, parent_subject) != 0) {
        ESP_LOGE(TAG, "Failed to reference ASN.1 data");
        mbedtls_x509_crt_free(new_cert);
        free(new_cert);
        return MBEDTLS_ERR_X509_ALLOC_FAILED;
    }

    child_issuer = child_issuer->next;
    while (child_issuer != NULL) {
        parent_subject->next = calloc(1, sizeof(mbedtls_asn1_named_data));
        if (parent_subject->next == NULL) {
            ESP_LOGE(TAG, "Failed to allocate memory for subject node");
            mbedtls_x509_crt_free(new_cert);
            free(new_cert);
            return MBEDTLS_ERR_X509_ALLOC_FAILED;
        }

        parent_subject = parent_subject->next;
        if (esp_crt_ref_asn1(child_issuer, parent_subject) != 0) {
            ESP_LOGE(TAG, "Failed to reference ASN.1 data");
            mbedtls_x509_crt_free(new_cert);
            free(new_cert);
            return MBEDTLS_ERR_X509_ALLOC_FAILED;
        }
        child_issuer = child_issuer->next;
    }]=])

foreach(_replacement IN ITEMS asn1_helper subject_raw issuer_copy)
    string(FIND "${_crt_bundle_contents}" "${_old_${_replacement}}"
        _replacement_offset)
    if(_replacement_offset EQUAL -1)
        message(FATAL_ERROR
            "ESP-IDF certificate-bundle backport could not find the audited "
            "${_replacement} block")
    endif()
    string(REPLACE "${_old_${_replacement}}" "${_new_${_replacement}}"
        _crt_bundle_contents "${_crt_bundle_contents}")
endforeach()

set(_patched_dir "${CMAKE_BINARY_DIR}/esp_idf_6_0_1_compat")
set(_patched_source "${_patched_dir}/esp_crt_bundle.c")
file(MAKE_DIRECTORY "${_patched_dir}")
file(WRITE "${_patched_source}" "${_crt_bundle_contents}")

idf_component_get_property(_mbedtls_lib mbedtls COMPONENT_LIB)
get_target_property(_mbedtls_sources ${_mbedtls_lib} SOURCES)
set(_filtered_mbedtls_sources "")
set(_original_source_found FALSE)
foreach(_source_entry IN LISTS _mbedtls_sources)
    string(REPLACE "\\" "/" _normalized_source_entry "${_source_entry}")
    if(_normalized_source_entry MATCHES
       "(^|/)esp_crt_bundle/esp_crt_bundle\\.c$")
        set(_original_source_found TRUE)
    else()
        list(APPEND _filtered_mbedtls_sources "${_source_entry}")
    endif()
endforeach()

if(NOT _original_source_found)
    message(FATAL_ERROR
        "Could not replace the ESP-IDF mbedTLS certificate-bundle source")
endif()

set_property(TARGET ${_mbedtls_lib} PROPERTY SOURCES
    "${_filtered_mbedtls_sources}")
target_sources(${_mbedtls_lib} PRIVATE "${_patched_source}")
message(STATUS
    "Applied ESP-IDF v6.0.1 cross-signed certificate-bundle leak backport")
