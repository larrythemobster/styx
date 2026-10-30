#ifdef __APPLE__

#include "SecureStore.hpp"

#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>

#include <limits>
#include <memory>
#include <type_traits>

namespace Styx::Platform {
namespace {

struct CFReleaser {
    void operator()(CFTypeRef object) const
    {
        if (object)
            CFRelease(object);
    }
};

template<typename T>
using CFPtr = std::unique_ptr<std::remove_pointer_t<T>, CFReleaser>;

CFPtr<CFStringRef> makeString(const std::string &value)
{
    if (value.size() > static_cast<size_t>(std::numeric_limits<CFIndex>::max()))
        return {};
    return CFPtr<CFStringRef>(CFStringCreateWithBytes(
        kCFAllocatorDefault, reinterpret_cast<const UInt8 *>(value.data()),
        static_cast<CFIndex>(value.size()), kCFStringEncodingUTF8, false));
}

CFPtr<CFMutableDictionaryRef> makeQuery(const std::string &service, const std::string &key)
{
    auto serviceString = makeString(service);
    auto accountString = makeString(key);
    if (!serviceString || !accountString)
        return {};

    CFPtr<CFMutableDictionaryRef> query(CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
    if (!query)
        return {};
    CFDictionarySetValue(query.get(), kSecClass, kSecClassGenericPassword);
    CFDictionarySetValue(query.get(), kSecAttrService, serviceString.get());
    CFDictionarySetValue(query.get(), kSecAttrAccount, accountString.get());
    return query;
}

} // namespace

bool SecureStore::writePassword(const std::string &service, const std::string &key, const std::string &password)
{
    if (password.size() > static_cast<size_t>(std::numeric_limits<CFIndex>::max()))
        return false;
    auto query = makeQuery(service, key);
    CFPtr<CFDataRef> data(CFDataCreate(kCFAllocatorDefault,
        reinterpret_cast<const UInt8 *>(password.data()), static_cast<CFIndex>(password.size())));
    CFPtr<CFMutableDictionaryRef> attributes(CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
    if (!query || !data || !attributes)
        return false;

    CFDictionarySetValue(attributes.get(), kSecValueData, data.get());
    OSStatus status = SecItemUpdate(query.get(), attributes.get());
    if (status == errSecItemNotFound) {
        CFDictionarySetValue(query.get(), kSecValueData, data.get());
        status = SecItemAdd(query.get(), nullptr);
        if (status == errSecDuplicateItem) {
            CFDictionaryRemoveValue(query.get(), kSecValueData);
            status = SecItemUpdate(query.get(), attributes.get());
        }
    }
    return status == errSecSuccess;
}

std::optional<std::string> SecureStore::readPassword(const std::string &service, const std::string &key)
{
    auto query = makeQuery(service, key);
    if (!query)
        return std::nullopt;
    CFDictionarySetValue(query.get(), kSecReturnData, kCFBooleanTrue);
    CFDictionarySetValue(query.get(), kSecMatchLimit, kSecMatchLimitOne);

    CFTypeRef result = nullptr;
    OSStatus status = SecItemCopyMatching(query.get(), &result);
    CFPtr<CFTypeRef> owned(result);
    if (status != errSecSuccess || !owned || CFGetTypeID(owned.get()) != CFDataGetTypeID())
        return std::nullopt;
    auto data = static_cast<CFDataRef>(owned.get());
    CFIndex length = CFDataGetLength(data);
    if (length == 0)
        return std::string();
    return std::string(reinterpret_cast<const char *>(CFDataGetBytePtr(data)), static_cast<size_t>(length));
}

bool SecureStore::deletePassword(const std::string &service, const std::string &key)
{
    auto query = makeQuery(service, key);
    if (!query)
        return false;
    OSStatus status = SecItemDelete(query.get());
    return status == errSecSuccess || status == errSecItemNotFound;
}

} // namespace Styx::Platform

#endif
