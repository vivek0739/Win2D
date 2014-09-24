// Copyright (c) Microsoft Corporation. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use these files except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
// WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
// License for the specific language governing permissions and limitations
// under the License.

#include "pch.h"
#include "CanvasEffect.h"

namespace ABI { namespace Microsoft { namespace Graphics { namespace Canvas { namespace Effects 
{
    CanvasEffect::CanvasEffect(IID effectId, unsigned int propertiesSize, unsigned int inputSize, bool isInputSizeFixed)
        : m_effectId(effectId)
    {
        m_properties = Make<Vector<IPropertyValue*>>(propertiesSize, true);
        CheckMakeResult(m_properties);

        m_inputs = Make<Vector<IEffectInput*>>(inputSize, isInputSizeFixed);
        CheckMakeResult(m_inputs);
        m_inputs->SetChanged(true);

        // Create propertyValueFactory
        HSTRING stringActivableClassId = Wrappers::HStringReference(RuntimeClass_Windows_Foundation_PropertyValue).Get();
        ThrowIfFailed(GetActivationFactory(stringActivableClassId, &m_propertyValueFactory));
    }

    //
    // ICanvasImageInternal
    //

    ComPtr<ID2D1Image> CanvasEffect::GetD2DImage(ID2D1DeviceContext* deviceContext)
    {
        CheckInPointer(deviceContext);

        // Check if device is the same as previous device
        bool wasRecreated = false;

        ComPtr<ID2D1Device> device;
        deviceContext->GetDevice(&device);

        ComPtr<IUnknown> deviceIdentity;
        ThrowIfFailed(device.As(&deviceIdentity));

        if (deviceIdentity != m_previousDeviceIdentity)
        {
            m_previousDeviceIdentity = deviceIdentity;
            m_resource.Reset();
            wasRecreated = true;
        }

        // Create resource if not created yet
        // TODO #802: make sure this lazy create is made properly threadsafe
        if (!m_resource)
            ThrowIfFailed(deviceContext->CreateEffect(m_effectId, &m_resource));

        // Update ID2D1Image with the latest property values if a change is detected
        if (wasRecreated || m_properties->IsChanged())
        {
            SetProperties();
            m_properties->SetChanged(false);
        }

        // Update ID2D1Image with the latest inputs configured, if a change is detected
        if (wasRecreated || m_inputs->IsChanged())
        {
            auto& inputs = m_inputs->InternalVector();
            auto inputsSize = (unsigned int) inputs.size();
            m_resource->SetInputCount(inputsSize);
            for (unsigned int i = 0; i < inputsSize; ++i)
            {
                if (!inputs[i])
                    ThrowHR(E_POINTER);

                ComPtr<ICanvasImageInternal> internalInput;
                HRESULT hr = inputs[i].As(&internalInput);

                if (hr == E_NOINTERFACE)
                {
                    WinStringBuilder message;
                    message.Format(Strings::EffectWrongInputType, i);
                    ThrowHR(hr, message.Get());
                }
                else
                {
                    ThrowIfFailed(hr);
                }

                m_resource->SetInput(i, internalInput->GetD2DImage(deviceContext).Get());
            }
            m_inputs->SetChanged(false);
        }

        ComPtr<ID2D1Image> image;
        ThrowIfFailed(m_resource.As(&image));
        return image;
    }

    //
    // IClosable
    //

    IFACEMETHODIMP CanvasEffect::Close()
    {
        m_resource.Reset();
        return S_OK;
    }

    //
    // IEffect
    //

    IFACEMETHODIMP CanvasEffect::get_EffectId(GUID* effectId)
    {
        CheckInPointer(effectId);

        *effectId = m_effectId;
        return S_OK;
    }

    IFACEMETHODIMP CanvasEffect::get_Inputs(_Out_ IVector<IEffectInput*>** inputs)
    {
        return ExceptionBoundary(
            [&]
            {
                CheckAndClearOutPointer(inputs);
                ThrowIfFailed(m_inputs.CopyTo(inputs));
            });
    }

    IFACEMETHODIMP CanvasEffect::get_Properties(_Out_ IVector<IPropertyValue*>** properties)
    {
        return ExceptionBoundary(
            [&]
            {
                CheckAndClearOutPointer(properties);
                ThrowIfFailed(m_properties.CopyTo(properties));
            });
    }

    void CanvasEffect::GetInput(unsigned int index, IEffectInput** input)
    {
        CheckAndClearOutPointer(input);

        ThrowIfFailed(m_inputs->GetAt(index, input));
    }

    void CanvasEffect::SetInput(unsigned int index, IEffectInput* input)
    {
        CheckInPointer(input);

        ComPtr<IEffectInput> effectInput;
        ThrowIfFailed(input->QueryInterface(effectInput.GetAddressOf()));
        ThrowIfFailed(m_inputs->SetAt(index, effectInput.Get()));
    }

    void CanvasEffect::SetProperties()
    {
        auto& properties = m_properties->InternalVector();
        auto propertiesSize = (unsigned int) properties.size();

        for (unsigned int i = 0; i < propertiesSize; ++i)
        {
            if (!properties[i])
                ThrowHR(E_POINTER);
            PropertyType propertyType;
            ThrowIfFailed(properties[i]->get_Type(&propertyType));

            switch (propertyType)
            {
            case PropertyType_UInt32:
            {
                UINT32 value;
                ThrowIfFailed(properties[i]->GetUInt32(&value));
                // TODO #2283: detailed exception error if failed
                ThrowIfFailed(m_resource->SetValue(i, value));
                break;
            }
            case PropertyType_Single:
            {
                float value;
                ThrowIfFailed(properties[i]->GetSingle(&value));
                // TODO #2283: detailed exception error if failed
                ThrowIfFailed(m_resource->SetValue(i, value));
                break;
            }
            case PropertyType_SingleArray:
            {
                float* value = nullptr;
                unsigned int size;
                ThrowIfFailed(properties[i]->GetSingleArray(&size, &value));

                auto freeArrayWarden = MakeScopeWarden([&] { CoTaskMemFree(value); });

                // Since d2d effects have input array based types:
                // D2D1_MATRIX_3X2_F, D2D1_MATRIX_4X4_F, D2D1_MATRIX_5X4_F, 
                // D2D1_VECTOR_2F, D2D1_VECTOR_3F, D2D1_VECTOR_4F
                // we can uniquely identify input type based on size
                // size 20 => D2D1_MATRIX_5X4_F
                // size 16 => D2D1_MATRIX_4X4_F
                // size 6  => D2D1_MATRIX_3X2_F
                // size 4  => D2D1_VECTOR_4F
                // size 3  => D2D1_VECTOR_3F
                // size 2  => D2D1_VECTOR_2F
                switch (size)
                {
                case 16:
                {
                    ThrowIfFailed(m_resource->SetValue(i, *(D2D1_MATRIX_4X4_F*)value));
                    break;
                }
                default:
                    ThrowHR(E_NOTIMPL);
                }
                break;
            }
            default:
                ThrowHR(E_NOTIMPL);
            }
        }
    }
}}}}}