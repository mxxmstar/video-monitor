<script setup lang="ts">
import { ref } from 'vue'
import { useRouter } from 'vue-router'
import {
  HomeOutlined,
  SettingOutlined,
  CameraOutlined,
  BellOutlined
} from '@ant-design/icons-vue'

const router = useRouter()

const selectedKeys = ref(['home'])
const openKeys = ref(['settings'])

const handleMenuClick = (e: any) => {
  if (e.key === 'home') {
    router.push('/home')
  } else if (e.key === 'video-source') {
    router.push('/settings/video-source')
  } else if (e.key === 'notification') {
    router.push('/settings/notification')
  }
}
</script>

<template>
  <a-layout style="min-height: 100vh">
    <a-layout-sider
      :width="240"
      theme="light"
      style="background: #f5f0f3; border-right: 1px solid #e8e0e5"
    >
      <div style="padding: 20px 16px; border-bottom: 1px solid #e8e0e5">
        <div style="display: flex; align-items: center; gap: 10px">
          <div style="width: 32px; height: 32px; background: linear-gradient(135deg, #e8c4d8, #d4a5c5); border-radius: 8px; display: flex; align-items: center; justify-content: center">
            <span style="color: white; font-size: 18px"></span>
          </div>
          <div>
            <div style="font-size: 18px; font-weight: 600; color: #4a3f55">Video Monitor</div>
            <div style="font-size: 12px; color: #8b7d96">视频监控智能平台</div>
          </div>
        </div>
      </div>

      <a-menu
        v-model:selectedKeys="selectedKeys"
        v-model:openKeys="openKeys"
        mode="inline"
        @click="handleMenuClick"
        style="background: transparent; border: none; padding: 8px"
      >
        <a-menu-item key="home">
          <HomeOutlined />
          <span>首页</span>
        </a-menu-item>

        <a-sub-menu key="settings">
          <template #title>
            <span>
              <SettingOutlined />
              <span>设置</span>
            </span>
          </template>
          <a-menu-item key="video-source">
            <CameraOutlined />
            <span>视频源管理</span>
          </a-menu-item>
          <a-menu-item key="notification">
            <BellOutlined />
            <span>通知管理</span>
          </a-menu-item>
        </a-sub-menu>
      </a-menu>
    </a-layout-sider>

    <a-layout style="height: 100vh; display: flex; flex-direction: column">
      <a-layout-header style="background: white; padding: 0 24px; display: flex; align-items: center; height: 56px; border-bottom: 1px solid #f0e8ed; flex-shrink: 0">
        <div>
          <div style="font-size: 18px; font-weight: 600; color: #4a3f55; line-height: 1.2">视频助手</div>
          <div style="font-size: 13px; color: #8b7d96; line-height: 1.2">欢迎回来，admin</div>
        </div>
      </a-layout-header>

      <a-layout-content style="padding: 24px; background: #faf7f9; overflow-y: auto; flex: 1">
        <router-view />
      </a-layout-content>
    </a-layout>
  </a-layout>
</template>

<style>
body {
  margin: 0;
  font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, 'Helvetica Neue', Arial, sans-serif;
}

.ant-menu-inline {
  border-inline-end: none !important;
}

.ant-menu-item,
.ant-menu-submenu-title {
  border-radius: 8px !important;
  margin: 4px 8px !important;
  width: calc(100% - 16px) !important;
}

.ant-menu-item-selected {
  background: #e8d5e0 !important;
  color: #4a3f55 !important;
}

.ant-menu-item:hover,
.ant-menu-submenu-title:hover {
  background: #f0e8ed !important;
}
</style>