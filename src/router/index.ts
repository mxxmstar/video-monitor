import { createRouter, createWebHashHistory } from 'vue-router'

const routes = [
    {
        path: '/',
        redirect: '/home'
    },
    {
        path: '/home',
        name: 'Home',
        component: () => import('../views/Home.vue'),
        meta: { title: '首页' }
    },
    {
        path: '/settings',
        name: 'Settings',
        redirect: '/settings/video-source',
        children: [
            {
                path: 'video-source',
                name: 'VideoSource',
                component: () => import('../views/settings/VideoSource.vue'),
                meta: { title: '视频源管理' }
            },
            {
                path: 'notification',
                name: 'Notification',
                component: () => import('../views/settings/Notification.vue'),
                meta: { title: '通知管理' }
            }
        ]
    }
]

const router = createRouter({
  history: createWebHashHistory(),
  routes
})

export default router